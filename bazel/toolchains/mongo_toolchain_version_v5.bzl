# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_PATCH_BUILD_ID = "N/A"
TOOLCHAIN_PATCH_BUILD_DATE = "N/A"
TOOLCHAIN_MAP_V5 = {
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "b85f8777a60f4a0ee130ff32e5ec040b843bafb0ce39b56145d5bdf9fa7c6490",
        "url": "https://mdb-build-public.s3.amazonaws.com/toolchains/aarch64_ubuntu2204_bazel_v5_toolchain.tar.gz",
    },
}
