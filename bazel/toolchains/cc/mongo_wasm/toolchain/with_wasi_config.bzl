load("//bazel/toolchains/cc/mongo_wasm/toolchain:wasi_transition.bzl", "wasi_transition")

def _with_wasi_config_impl(ctx):
    return [DefaultInfo(files = depset(ctx.files.srcs))]

with_wasi_config = rule(
    implementation = _with_wasi_config_impl,
    attrs = {
        "srcs": attr.label_list(
            cfg = wasi_transition,
            # Pass all files through the transition
            allow_files = True,
        ),
    },
)
