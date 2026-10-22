# nugget-build.sh

Build Nugget binaries from an app's normal build: the analysis binary that
writes `analysis-output.csv`, and per-region binaries with gem5 m5 ops at the
region's markers.

```sh
NB=/lustre/orion/csc688/world-shared/nugget-build/nugget-build.sh

# 1. Build the app once through the script. CMake:
$NB cmake -S . -B build <your usual cmake flags>
cmake --build build -j 16
#    Make:  make CC="$NB wrap clang" CXX="$NB wrap clang++"

# 2. Analysis binary (interval = IR instructions per region), then run it like the app:
$NB analysis build/app 1000000
./build/app.analysis <args>              # -> analysis-output.csv

# 3. Instrumentation run: same regions, records each region's wall time and GPU energy:
$NB energy build/app 1000000
./build/app.energy <args>                # -> energy-output.csv
#    Cluster on the analysis run, predict time and energy from the representatives:
./cluster.py analysis-output.csv --measure energy-output.csv -k 12

# 4. (gem5) One checkpoint at the start of each representative region, in one run:
$NB checkpoint build/app 1000000          # same interval as the analysis binary
NUGGET_REGIONS=0,42,65,211 <gem5 ...> build/app.checkpoint <args>   # -> cpt.<tick>/ per region,
                                          #    checkpoint-log.csv maps them to regions
#    (or, with markers instead of counting, one binary per region:)
export M5_INCLUDE=/path/to/gem5/include M5_LIB=/path/to/gem5/util/m5/build/x86/out/libm5.a
$NB region build/app analysis-output.csv 42        # -> build/app.region-42
$NB region build/app analysis-output.csv 42 2      # same, with 2 warm-up regions before it
```

- **ROI.** The app must call `nugget_roi_begin_()` / `nugget_roi_end_()`
  around its region of interest, as before.
- **Compilers.** Use clang-family compilers (clang, amdclang, Cray `CC` with
  PrgEnv-amd). Files that can't produce bitcode (e.g. Cray Fortran) are
  reported during the build and stay uninstrumented.
- **HIP sources are not instrumented** (`-x hip`, `.hip`, `.cu`, hipcc,
  `--offload-arch`), as in the original Nugget flow. Their host code waits on
  the GPU (rocHPL's panel factorization spins on a flag the running kernel
  sets), so its instruction count depends on timing. GPU activity enters only
  through the library interceptor (tapir `gpu_*` rows). Run with one rank and
  `OMP_NUM_THREADS=1`: the hooks are single-threaded.
- **Interval.** The region length is compiled in (`analysis`/`energy`
  argument); build both binaries with the same interval. The GPU energy
  counter updates about every 8 ms, so regions much shorter than that report
  energy only in coarse steps.
- **gem5 actions** are in `hooks/gem5.c`, so edit them there:
  - warm-up marker: `m5_checkpoint`;
  - start marker: `m5_checkpoint` (if there was no warm-up marker), then `m5_reset_stats`;
  - end marker: `m5_dump_stats`, then `m5_exit`.

  Test on real hardware with `NUGGET_NO_M5=1`, which builds a hook that
  prints instead of calling m5.
- **Markers** are chosen from the CSV with upstream Nugget's margin rule: the
  least executed block near each region's end. Such blocks have counts that
  don't depend on timing, which matters for codes that poll the GPU.
- **Defaults and overrides.** LLVM defaults to ROCm's (`LLVM=` to override).
  The passes in `passes/` are the upstream Nugget passes with fixes; they are
  built on first use into `build/`.
- **Files.**
  - `hooks/analysis.c` is the GPU-aware analysis hook. With tapir preloaded it
    also writes `gpu_bb_id`/`gpu_bbv` rows. It writes each region when the
    region closes and keeps only the current region's counts (memory is
    O(basic blocks); the original buffered 10,000 regions, 60 GB for QMCPACK).
  - `hooks/energy.c` is the instrumentation hook: wall time and GPU energy per region.
  - `cluster.py` and `notebook.py` cluster the analysis run and predict time and
    energy; `results/<app>/` has the runs of rocHPL, rocHPL-MxP, HPG-MxP, LSMS
    and QMCPACK, each with its notebook and `repro/` (ROI patch, build and run scripts).
  - Build products go next to the executable: `<exe>.nugget/`, `<exe>.link`,
    `<exe>.analysis`, `<exe>.region-<r>`.
