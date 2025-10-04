# SelectiveSanitizer (SelSan)

`SelSan` is implementation of
[HWASAN](https://clang.llvm.org/docs/HardwareAssistedAddressSanitizerDesign.html)
runtime in `TCMalloc`. `SelSan` allows to use `HWASAN` runtime checking for
production applications that use/rely on `TCMalloc`.

This is currently work-in-progress.

`SelSan` requires either Arm TBI (top byte ignore), or Intel LAM (linear address
masking), or AMD UAI (upper address ignore) CPU features. `SelSan` can also be
used with Arm MTE (memory tagging extension).

Reference compiler flags required for `SelSan` are:

```
-DTCMALLOC_UNDER_SANITIZERS=0
-DTCMALLOC_INTERNAL_SELSAN=1
-fsanitize=hwaddress
-mllvm -hwasan-globals=0
-mllvm -hwasan-with-tls=0
-mllvm -hwasan-instrument-mem-intrinsics=0
-mllvm -hwasan-memory-access-callback-prefix=__selsan_
-mllvm -hwasan-mapping-offset=0
-lselsan
```
