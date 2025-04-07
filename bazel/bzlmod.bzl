load("@bazel_features//:deps.bzl", _bazel_features_deps = "bazel_features_deps")
load("//bazel/platforms:local_config_platform.bzl", "setup_local_config_platform")
load("//bazel/toolchains/python:python_toolchain.bzl", _setup_mongo_python_toolchains = "setup_mongo_python_toolchains")
load("//bazel/toolchains/cc/mongo_linux:mongo_toolchain.bzl", _setup_mongo_toolchains = "setup_mongo_toolchains")

def _bazel_features_deps_impl(_ctx):
    _bazel_features_deps()

bazel_features_deps = module_extension(
    implementation = _bazel_features_deps_impl,
)

def _setup_mongo_python_toolchains_impl(_ctx):
    _setup_mongo_python_toolchains()

setup_mongo_python_toolchains = module_extension(
    implementation = _setup_mongo_python_toolchains_impl,
)

def _setup_mongo_toolchains_impl(_ctx):
    setup_local_config_platform(name = "internal_platforms_do_not_use")
    _setup_mongo_toolchains()

setup_mongo_toolchains = module_extension(
    implementation = _setup_mongo_toolchains_impl,
)
