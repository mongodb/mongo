# Use mongo/bazel/toolchains/toolchain_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_URL_FORMAT = "https://mciuploads.s3.amazonaws.com/toolchain-builder/{platform_name}/{patch_build_id}/bazel_v4_toolchain_builder_{underscore_platform_name}_{patch_build_id}_{patch_build_date}.tar.gz"
TOOLCHAIN_PATCH_BUILD_ID = "11bae3c145a48dd7be9ee8aa44e5591783f787aa"
TOOLCHAIN_PATCH_BUILD_DATE = "24_01_09_16_10_07"
TOOLCHAIN_MAP = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "521b80a87a199eb0a0a148f92b4140ff161cd3cf7bf813d58f650055f4ff8523",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/amazon2023-arm64/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_amazon2023_arm64_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "269e54f97d9049d24d934f549a8963c15c954a5cb6fc0d75bbbcfb78df3c3647",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/amazon2-arm64/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_amazon2_arm64_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "e2bf59dacb789bd3ed708bafb7bf4e432f611f19d6b888340e3b73eee6949b31",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/amazon2/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_amazon2_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "94b67aecc97501a314214c6c8dd00008edeb139f33097180cea70ffef623dc61",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel80/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_rhel80_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "efb59b97ac16150a91ad76609bb5ecfe644a5140bc08bd123c0bd44fea78a3a0",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel80-zseries/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_rhel80_zseries_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "e9ac010977f6b92d301174a5749c06e4678a0071556745ea3681a2825b6f7bd1",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel81-ppc64le/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_rhel81_ppc64le_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "72c9d731021b8e42096bd04d0fbf53254062f5ec1830f8707ebbcc73ea29a0b3",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/ubuntu2204-arm64/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_ubuntu2204_arm64_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "04ca222c288d6601a8cbc22e1d75f2170cbc24e77dfb59e27249a78872952f16",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/ubuntu2204/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_ubuntu2204_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
}
