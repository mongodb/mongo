load("@bazel_features//:deps.bzl", _bazel_features_deps = "bazel_features_deps")
load("//bazel/toolchains/python:python_toolchain.bzl", _setup_mongo_python_toolchains = "setup_mongo_python_toolchains")
load("//bazel/toolchains/cc/mongo_linux:mongo_gdb.bzl", _setup_gdb_toolchains = "setup_gdb_toolchains")
load("//bazel/toolchains/cc/mongo_linux:mongo_toolchain.bzl", _setup_mongo_toolchains = "setup_mongo_toolchains")

def _bazel_features_deps_impl(_ctx):
    _bazel_features_deps()

bazel_features_deps = module_extension(
    implementation = _bazel_features_deps_impl,
)

def _setup_mongo_python_toolchains_impl(ctx):
    _setup_mongo_python_toolchains(ctx)

setup_mongo_python_toolchains = module_extension(
    implementation = _setup_mongo_python_toolchains_impl,
)

def _setup_mongo_toolchains_impl(_ctx):
    _setup_mongo_toolchains()
    _setup_gdb_toolchains()

# NOTE: the extension is named `setup_mongo_toolchains` on purpose. Repos
# created by a module extension are canonically named
# `_main~<extension>~<repo>`, so this spelling is what makes the toolchain land
# at `external/_main~setup_mongo_toolchains~mongo_toolchain_v5`. Several
# out-of-Bazel consumers hardcode that path (buildscripts/clangd_vscode.sh,
# .devcontainer/devcontainer.json, .vscode_defaults/*.code-workspace), so
# renaming this symbol means updating them too.
setup_mongo_toolchains = module_extension(
    implementation = _setup_mongo_toolchains_impl,
)
