# Usage

To use the WASI SDK apply the `wasi_compatible` with a select statement:

```python
    select({
        "//bazel/toolchains/cc/wasm:wasi_compatible": [
            "//bazel/toolchains/cc/wasm/sample:dist_hello_world",
        ],
        "//conditions:default": [],
    })
```

If your target is defined in terms of a traditional bazel C/C++ target you can use the WASI transition in order to ensure the bazel options are WASI compatible.

```python
load("//bazel/toolchains/cc/wasm/toolchain:with_wasi_config.bzl", "with_wasi_config")
with_wasi_config(
    name = "dist_hello_world",
    srcs = [
        ":hello_world",
    ],
)
```
