# shadGT patches for the native Linux build

The native Linux renderer is shadGT (https://github.com/mlgprorektm8/shadGT) at commit
`afc9e36c` with these patches applied, in order:

```sh
git clone https://github.com/mlgprorektm8/shadGT && cd shadGT
git checkout -b linux-native afc9e36c
git am /path/to/RyGT/tools/linux/shadgt-patches/*.patch
```

Build (clang): `cmake -B build -G Ninja -DSHADGT_RYTY_GPU=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON`,
then build the `*.prx` targets and copy them into the native game folder's `libs/`.

Diagnostics added here: DIAG-109 (SHADGT_DIAG_OBTAIN=1) obtain sizes, DIAG-142 unresolved guarded
reads, DIAG-143 (SHADGT_FORCE_UPLOAD=1) re-uploads on every use. PERF-154 (SHADGT_WRITE_SCAN=1)
is experimental and currently too slow.
