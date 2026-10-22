#!/usr/bin/env bash
# nugget-build.sh: build Nugget analysis and gem5 instrumentation binaries
# from an app's normal build.
#
#   1. Build the app once with this script as compiler launcher. Each object
#      also gets LLVM bitcode, and the link command is recorded. HIP sources
#      (-x hip, .hip, hipcc, --offload-arch) get none and stay uninstrumented:
#        nugget-build.sh cmake -S . -B build <your flags>   # CMake (then build as usual)
#        make CC="nugget-build.sh wrap clang" CXX="nugget-build.sh wrap clang++"
#   2. nugget-build.sh analysis <exe> <interval>
#        -> <exe>.analysis. Run it like the app; it writes analysis-output.csv
#           (hooks/analysis.c; NUGGET_ANALYSIS_OUTPUT sets the path).
#   3. nugget-build.sh energy <exe> <interval>
#        -> <exe>.energy: same regions, records each region's wall time and GPU
#           energy (hooks/energy.c) to energy-output.csv. Use the same interval.
#      cluster.py analysis-output.csv --measure energy-output.csv -k K
#        clusters on the analysis run, predicts time/energy from representatives.
#   4. nugget-build.sh checkpoint <exe> <interval>
#        -> <exe>.checkpoint: run under gem5 with NUGGET_REGIONS=r1,r2,... to
#           get one checkpoint at the start of each representative region.
#   nugget-build.sh object <exe> <interval> <hook.c> <out.o>
#        -> the instrumented program (all instrumented code + hook) as one
#           object, to link with another platform's GPU code (hooks/sim.c
#           for GPGPU-Sim).
#   5. (markers) Pick regions from the CSV (e.g. the simpoint notebook), then for each:
#      nugget-build.sh region <exe> <analysis-output.csv> <region> [warmup-regions]
#        -> <exe>.region-<r> with hooks/gem5.c at that region's markers.
#
# The app must call nugget_roi_begin_() / nugget_roi_end_() around its region
# of interest (as before). Environment:
#   LLVM           LLVM root (default: $ROCM_PATH/lib/llvm, /opt/rocm/lib/llvm, newest /opt/rocm-*)
#   M5_INCLUDE     dir containing gem5/m5ops.h     (region builds)
#   M5_LIB         libm5.a to link                 (region builds)
#   NUGGET_NO_M5=1 region build prints instead of calling m5 ops (for testing)
#   NUGGET_EXCLUDE regex of source paths not to instrument (wrap), e.g. an app's
#                  CUDA/HIP dispatch layer, so regions match across platforms
set -euo pipefail

HERE=$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
SELF="$HERE/nugget-build.sh"
LLVM=${LLVM:-${ROCM_PATH:+$ROCM_PATH/lib/llvm}}
[[ -z "$LLVM" && -d /opt/rocm/lib/llvm ]] && LLVM=/opt/rocm/lib/llvm
[[ -z "$LLVM" ]] && LLVM=$(ls -d /opt/rocm-*/lib/llvm 2>/dev/null | sort -V | tail -1)
[[ -x "$LLVM/bin/opt" ]] || { echo "nugget-build: no LLVM at '$LLVM'; set LLVM=" >&2; exit 1; }
PLUGIN="$HERE/build/$(basename "$(readlink -f "$LLVM")")-$(echo "$LLVM" | md5sum | cut -c1-8)/NuggetPasses.so"

die() { echo "nugget-build: $*" >&2; exit 1; }
say() { echo "nugget-build: $*" >&2; }

# ---- compiler launcher --------------------------------------------------------

has_bc_section() { "$LLVM/bin/llvm-readelf" -S "$1" 2>/dev/null | grep -q '\.nugget_bc'; }

wrap() {
    # Nested calls (mpicc -> clang) and our own extra compiles pass through.
    if [[ -n "${NUGGET_WRAP_ACTIVE:-}" ]]; then exec "$@"; fi
    export NUGGET_WRAP_ACTIVE=1
    "$@" || exit $?
    local cc=$1; shift
    local args=("$@") a i out="" compile=0 link=1
    for a in "${args[@]}"; do
        case "$a" in
            -c) compile=1 ;;
            -E|-S|-M|-MM|-shared|-r|--version|-v|-print-*) link=0 ;;
        esac
    done
    for ((i = 0; i < ${#args[@]}; i++)); do
        [[ "${args[i]}" == -o ]] && out=${args[i+1]:-}
    done
    if ((compile)); then
        # Same command, minus -c/-o/dependency files and the sources.
        local flags=() srcs=() skip=0 hip=0
        for ((i = 0; i < ${#args[@]}; i++)); do
            a=${args[i]}
            if ((skip)); then skip=0; continue; fi
            case "$a" in
                -o|-MF|-MT|-MQ) skip=1; continue ;;
                -c|-MD|-MMD|-MP|-save-temps*|-flto*) continue ;;
                --offload-arch*|--cuda-gpu-arch*|-fgpu-rdc) hip=1 ;;
            esac
            [[ "$a" == hip || "$a" == cuda ]] && [[ "${args[i-1]:-}" == -x ]] && hip=1
            if [[ "$a" != -* && "$a" =~ \.(c|cc|cpp|cxx|C|cu|hip|f|f90|F90)$ && -f "$a" ]]; then
                srcs+=("$a"); continue
            fi
            flags+=("$a")
        done
        [[ "$(basename "$cc")" == hipcc* ]] && hip=1
        local src obj bc tmp host
        for src in "${srcs[@]}"; do
            obj=$out
            [[ -z "$obj" || ${#srcs[@]} -gt 1 ]] && obj="$(basename "${src%.*}").o"
            [[ -f "$obj" ]] || continue
            bc="$(readlink -f "$obj").bc"
            if ((hip)) || [[ "$src" == *.hip || "$src" == *.cu ]] ||
               [[ -n "${NUGGET_EXCLUDE:-}" && "$src" =~ $NUGGET_EXCLUDE ]]; then
                # HIP sources are not instrumented (as in the original Nugget
                # flow): their host code waits on the GPU, so its instruction
                # count depends on timing. GPU activity is seen through the
                # library interceptor (tapir) instead.
                # NUGGET_EXCLUDE (regex on the source path): other platform-
                # specific GPU code, e.g. an app's CUDA/HIP dispatch layer, so
                # the instrumented code is the same on every platform.
                continue
            else
                "$cc" "${flags[@]}" -g0 -emit-llvm -c "$src" -o "$bc" >/dev/null 2>&1 || rm -f "$bc"
            fi
            if [[ -f "$bc" ]]; then
                printf '%s\n' "$bc" > "$bc.sec"
                "$LLVM/bin/llvm-objcopy" --add-section .nugget_bc="$bc.sec" "$obj" 2>/dev/null ||
                    "$LLVM/bin/llvm-objcopy" --update-section .nugget_bc="$bc.sec" "$obj"
                rm -f "$bc.sec"
            else
                echo "nugget-build: no bitcode for $src (stays uninstrumented)" >&2
            fi
        done
    elif ((link)) && [[ -n "$out" ]]; then
        # Record the link command; `analysis`/`region` replay it.
        { printf 'cd %q\n' "$PWD"; printf '%q ' "$cc" "$@"; echo; } > "$(readlink -f "$out").link"
    fi
}

# FindHIP (find_package(HIP)) replaces the HIP link rule with its own
# hipcc_cmake_linker_helper; route that helper through the wrapper so links
# of FindHIP projects are recorded too.
hip_link_helper() {
    local real="$LLVM/../../bin/hipcc_cmake_linker_helper" d="$HERE/build/hip"
    [[ -x "$real" ]] || return 0
    mkdir -p "$d"
    printf '#!/bin/sh\nexec %q wrap %q "$@"\n' "$SELF" "$(readlink -f "$real")" > "$d/hipcc_cmake_linker_helper"
    chmod +x "$d/hipcc_cmake_linker_helper"
    CMAKE_FLAGS+=("-DHIP_HIPCC_CMAKE_LINKER_HELPER=$d/hipcc_cmake_linker_helper")
}

cmake_flags() {
    local l
    CMAKE_FLAGS=()
    hip_link_helper
    for l in C CXX HIP Fortran; do CMAKE_FLAGS+=("-DCMAKE_${l}_COMPILER_LAUNCHER=$SELF;wrap"); done
    # Links: CMake applies linker launchers to only some languages, so wrap
    # the link rules (CMake sets its defaults only when these are unset).
    for l in C CXX HIP; do
        CMAKE_FLAGS+=("-DCMAKE_${l}_LINK_EXECUTABLE=$SELF wrap <CMAKE_${l}_COMPILER> <FLAGS> <CMAKE_${l}_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
    done
}

# ---- passes -----------------------------------------------------------------------

setup() {
    [[ -f "$PLUGIN" ]] && return
    say "building the Nugget passes against $LLVM"
    cmake -S "$HERE/passes" -B "$(dirname "$PLUGIN")" -DLLVM_DIR="$LLVM/lib/cmake/llvm" \
        -DCMAKE_CXX_COMPILER="$LLVM/bin/clang++" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$(dirname "$PLUGIN")" -j 8 >/dev/null
}

# ---- bitcode -> binary ------------------------------------------------------------

# labeled.bc: the whole program's bitcode (listed in the exe's .nugget_bc
# section), optimized and with basic-block ids. Shared by every build so the
# ids in the CSV match the region binaries.
labeled() {
    local exe=$1 work=$2
    [[ -f "$work/labeled.bc" && "$work/labeled.bc" -nt "$exe" ]] && return
    mkdir -p "$work"
    "$LLVM/bin/llvm-objcopy" --dump-section .nugget_bc="$work/bitcode.list" "$exe" /dev/null 2>/dev/null ||
        die "$exe has no bitcode; build it with nugget-build.sh as compiler launcher"
    sort -u "$work/bitcode.list" | grep . > "$work/bitcode.sorted"
    say "linking $(wc -l < "$work/bitcode.sorted") bitcode files"
    "$LLVM/bin/llvm-link" $(cat "$work/bitcode.sorted") -o "$work/whole.bc"
    "$LLVM/bin/opt" -load-pass-plugin="$PLUGIN" \
        -passes="default<O2>,ir-bb-label-pass<output_csv=$work/bb-info.csv>" "$work/whole.bc" -o "$work/labeled.bc"
}

# Replay the recorded link with the tagged objects replaced by $1.
relink() {
    local exe=$1 newobj=$2 output=$3; shift 3
    local extra=("$@") work=$(dirname "$newobj")
    [[ -f "$exe.link" ]] || die "no $exe.link; the link did not go through nugget-build.sh"
    local cwd cmd
    cwd=$(head -1 "$exe.link" | sed 's/^cd //')
    eval "cmd=($(tail -n +2 "$exe.link"))"
    cwd=$(eval echo "$cwd")
    local new=() a i=0 inserted=0
    pushd "$cwd" >/dev/null
    # Expand @response files.
    local expanded=()
    for a in "${cmd[@]:1}"; do
        if [[ "$a" == @* && -f "${a:1}" ]]; then eval "expanded+=($(cat "${a:1}"))"; else expanded+=("$a"); fi
    done
    for ((i = 0; i < ${#expanded[@]}; i++)); do
        a=${expanded[i]}
        if [[ "$a" == -o ]]; then new+=(-o "$output"); i=$((i + 1)); continue; fi
        if [[ -f "$a" && "$a" == *.o ]] && has_bc_section "$a"; then
            ((inserted)) || new+=("$newobj")
            inserted=1; continue
        fi
        if [[ -f "$a" && "$a" == *.a ]]; then new+=("$(filter_archive "$a" "$work")"); continue; fi
        new+=("$a")
    done
    ((inserted)) || new=("$newobj" "${new[@]}")
    NUGGET_WRAP_ACTIVE=1 "${cmd[0]}" "${new[@]}" "${extra[@]}"
    popd >/dev/null
}

# A copy of archive $1 without members that carry bitcode (their code is in
# the instrumented object), or $1 itself if it has none.
filter_archive() {
    local lib=$1 work=$2 tmp m keep=() dropped=0
    tmp=$(mktemp -d "$work/ar.XXXXXX")
    (cd "$tmp" && "$LLVM/bin/llvm-ar" x "$(readlink -f "$OLDPWD/$lib" 2>/dev/null || readlink -f "$lib")")
    for m in "$tmp"/*; do
        if has_bc_section "$m"; then dropped=1; else keep+=("$m"); fi
    done
    if ((dropped)); then
        local out="$work/$(basename "$lib")"
        rm -f "$out"
        "$LLVM/bin/llvm-ar" rcs "$out" "${keep[@]}"
        echo "$out"
    else
        echo "$lib"
    fi
}

# hook source -> bitcode, merged after labeling so the hooks get no ids.
build_variant() {
    local exe=$1 work=$2 hook=$3 passes=$4 output=$5; shift 5
    local hook_flags=("$@") extra=()
    "$LLVM/bin/clang" -O0 -emit-llvm -c "$hook" -I"$HERE/hooks" "${hook_flags[@]}" -o "$work/hook.bc"
    "$LLVM/bin/llvm-link" "$work/labeled.bc" "$work/hook.bc" -o "$work/hooked.bc"
    "$LLVM/bin/opt" -load-pass-plugin="$PLUGIN" -passes="$passes" "$work/hooked.bc" -o "$work/pass.bc" 2>"$work/opt.log" ||
        { cat "$work/opt.log" >&2; die "opt failed"; }
    "$LLVM/bin/llc" -O2 -filetype=obj --relocation-model=pic "$work/pass.bc" -o "$work/nugget.o"
    [[ -n "${M5_LIB:-}" && ( "$hook" == *gem5.c || "$hook" == *checkpoint.c ) && -z "${NUGGET_NO_M5:-}" ]] && extra+=("$M5_LIB")
    relink "$exe" "$work/nugget.o" "$output" "${extra[@]}" -ldl
    say "wrote $output"
}

analysis() {
    local exe interval=${2:-}
    exe=$(readlink -f "${1:?usage: analysis <exe> <interval>}")
    [[ -n "$interval" ]] || die "usage: analysis <exe> <interval>"
    setup
    local work="$exe.nugget"
    labeled "$exe" "$work"
    build_variant "$exe" "$work" "$HERE/hooks/analysis.c" "phase-analysis-pass<interval_length=$interval>" "$exe.analysis"
}

# Markers for one region from the analysis CSV: the end of a region is the
# least executed block among those last run near its end (upstream Nugget's
# margin rule, widened when only loop blocks are there), so its count does not
# depend on timing. Counts are cumulative since the ROI began.
markers() {
    python3 - "$@" <<'EOF'
import sys
csv, region, warm = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
regs = {}
for line in open(csv):
    p = line.rstrip("\n").split(",")
    if p[0] in ("bbv", "csv", "bb_id") and len(p) > 2 and p[1].isdigit():
        regs.setdefault(int(p[1]), {})[p[0]] = [int(x) for x in p[3:] if x]
n = max(regs) + 1
def count(bb, last):
    return sum(r.get("bbv", [])[r["bb_id"].index(bb)] for i, r in regs.items()
               if i <= last and bb in r.get("bb_id", []))
def end_of(r):
    g = regs[r]; st, ids, cnt = g["csv"], g["bb_id"], g["bbv"]
    top = max(st)
    for m in (99, 95, 90, 75, 50, 0):
        cands = [i for i, s in enumerate(st) if s >= top * m / 100.0]
        i = min(cands, key=lambda i: (cnt[i], -st[i]))
        if cnt[i] <= 64:
            break
    return ids[i], count(ids[i], r)
if not 0 <= region < n:
    sys.exit("region %d not in %s (0..%d)" % (region, csv, n - 1))
end = end_of(region) if region < n - 1 else (None, 0)
start = end_of(region - 1) if region > 0 else (None, 0)
w = region - 1 - warm
wu = end_of(w) if warm > 0 and w >= 0 else (None, 0)
any_bb = next(b for b, _ in (end, start, wu, (regs[0]["bb_id"][0], 0)) if b is not None)
f = lambda m: (m[0] if m[0] is not None else any_bb, m[1])
(wb, wc), (sb, sc), (eb, ec) = f(wu), f(start), f(end)
print("phase-bound-pass<warmup_marker_bb_id=%d;warmup_marker_count=%d;start_marker_bb_id=%d;"
      "start_marker_count=%d;end_marker_bb_id=%d;end_marker_count=%d>" % (wb, wc, sb, sc, eb, ec))
EOF
}

energy() {
    local exe interval=${2:-}
    exe=$(readlink -f "${1:?usage: energy <exe> <interval>}")
    [[ -n "$interval" ]] || die "usage: energy <exe> <interval>"
    setup
    local work="$exe.nugget"
    labeled "$exe" "$work"
    mkdir -p "$work/energy"
    cp "$work/labeled.bc" "$work/energy/labeled.bc"
    build_variant "$exe" "$work/energy" "$HERE/hooks/energy.c" "phase-analysis-pass<interval_length=$interval>" "$exe.energy"
}

# One binary that takes a gem5 checkpoint at the start of every region listed
# in $NUGGET_REGIONS at run time (hooks/checkpoint.c). Same pass and interval
# as `analysis`, so region r starts where it did in the analysis run.
checkpoint() {
    local exe interval=${2:-} flags=()
    exe=$(readlink -f "${1:?usage: checkpoint <exe> <interval>}")
    [[ -n "$interval" ]] || die "usage: checkpoint <exe> <interval>"
    setup
    local work="$exe.nugget"
    labeled "$exe" "$work"
    mkdir -p "$work/checkpoint"
    cp "$work/labeled.bc" "$work/checkpoint/labeled.bc"
    if [[ -n "${NUGGET_NO_M5:-}" ]]; then
        flags+=(-DNUGGET_NO_M5)
    else
        [[ -n "${M5_INCLUDE:-}" && -n "${M5_LIB:-}" ]] || die "set M5_INCLUDE and M5_LIB (or NUGGET_NO_M5=1 to test)"
        flags+=(-I"$M5_INCLUDE")
    fi
    build_variant "$exe" "$work/checkpoint" "$HERE/hooks/checkpoint.c" \
        "phase-analysis-pass<interval_length=$interval>" "$exe.checkpoint" "${flags[@]}"
}

# The instrumented program as one object file, with any hook and no link:
# for linking against another platform's GPU code (e.g. a CUDA build of the
# same HIP sources), since the instrumented host code is platform-neutral.
object() {
    local exe interval=${2:-} hook=${3:-} out=${4:-}
    exe=$(readlink -f "${1:?usage: object <exe> <interval> <hook.c> <out.o>}")
    [[ -n "$interval" && -f "$hook" && -n "$out" ]] || die "usage: object <exe> <interval> <hook.c> <out.o>"
    setup
    local work="$exe.nugget" name
    name=$(basename "$hook" .c)
    labeled "$exe" "$work"
    mkdir -p "$work/object-$name"
    "$LLVM/bin/clang" -O0 -emit-llvm -c "$hook" -I"$HERE/hooks" -o "$work/object-$name/hook.bc"
    "$LLVM/bin/llvm-link" "$work/labeled.bc" "$work/object-$name/hook.bc" -o "$work/object-$name/hooked.bc"
    "$LLVM/bin/opt" -load-pass-plugin="$PLUGIN" -passes="phase-analysis-pass<interval_length=$interval>" \
        "$work/object-$name/hooked.bc" -o "$work/object-$name/pass.bc" 2>"$work/object-$name/opt.log" ||
        { cat "$work/object-$name/opt.log" >&2; die "opt failed"; }
    "$LLVM/bin/llc" -O2 -filetype=obj --relocation-model=pic "$work/object-$name/pass.bc" -o "$out"
    say "wrote $out"
}

region() {
    local exe csv=${2:-} r=${3:-} warm=${4:-0}
    exe=$(readlink -f "${1:?usage: region <exe> <analysis-output.csv> <region> [warmup-regions]}")
    [[ -f "$csv" && -n "$r" ]] || die "usage: region <exe> <analysis-output.csv> <region> [warmup-regions]"
    setup
    local work="$exe.nugget" passes flags=()
    labeled "$exe" "$work"
    passes=$(markers "$csv" "$r" "$warm")
    say "region $r: $passes"
    mkdir -p "$work/region-$r"
    cp "$work/labeled.bc" "$work/region-$r/labeled.bc"
    if [[ -n "${NUGGET_NO_M5:-}" ]]; then
        flags+=(-DNUGGET_NO_M5)
    else
        [[ -n "${M5_INCLUDE:-}" && -n "${M5_LIB:-}" ]] || die "set M5_INCLUDE and M5_LIB (or NUGGET_NO_M5=1 to test)"
        flags+=(-I"$M5_INCLUDE")
    fi
    build_variant "$exe" "$work/region-$r" "$HERE/hooks/gem5.c" "$passes" "$exe.region-$r" "${flags[@]}"
}

case "${1:-}" in
    wrap) shift; wrap "$@" ;;
    cmake) shift; cmake_flags; exec cmake "$@" "${CMAKE_FLAGS[@]}" ;;
    cmake-flags) cmake_flags; printf '%q ' "${CMAKE_FLAGS[@]}"; echo ;;
    setup) setup; echo "$PLUGIN" ;;
    analysis) shift; analysis "$@" ;;
    energy) shift; energy "$@" ;;
    checkpoint) shift; checkpoint "$@" ;;
    object) shift; object "$@" ;;
    region) shift; region "$@" ;;
    *) sed -n '2,30p' "$SELF" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
