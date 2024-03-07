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
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "afaccbfd377c8f0ca4edc4396dd93bcfb816c302e2a446ba01062a6d8b3de433",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/amazon2023/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_amazon2023_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
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
    "debian10_x86_64": {
        "platform_name": "debian10",
        "sha": "f36060ae5ff9c2f1a8ec5009cbaaa7e7f1f12fd0001a1f37a7d61046e94c4acf",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/debian10/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_debian10_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "382aeec8de641d466e2ca2562539b497534c620e0e403304d6794434e73bbdce",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/debian12/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_debian12_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "rhel7_x86_64": {
        "platform_name": "rhel70",
        "sha": "ad7632e3fe32b336ebd0529ba3f865d4432028c6e3633325c910f1f98c8be542",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel70/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_rhel70_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "5e7d9d0b9f978332c409359453acacec4ce1106030674e549203a0c61a049bc4",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel82-arm64/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_rhel82_arm64_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "e9ac010977f6b92d301174a5749c06e4678a0071556745ea3681a2825b6f7bd1",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel81-ppc64le/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_rhel81_ppc64le_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "efb59b97ac16150a91ad76609bb5ecfe644a5140bc08bd123c0bd44fea78a3a0",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel80-zseries/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_rhel80_zseries_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "94b67aecc97501a314214c6c8dd00008edeb139f33097180cea70ffef623dc61",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel80/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_rhel80_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "de77d0a9d4f1b59ffc2e458c9c6f8f87093cd245c53bbcd50990a9c1f152aa2b",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel90-arm64/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_rhel90_arm64_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "ee596be40aa6ab5a676ce700ddd20a5fa05b2b311ae2c56f756b96def74102c4",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel90/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_rhel90_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "084a0a24949f8a20aeb2f1108e284c8304602544d40a3f0087de869dcf5faf93",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/suse15/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_suse15_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "ubuntu18_x86_64": {
        "platform_name": "ubuntu1804",
        "sha": "526b64a04731275ae52b9d8b062efa20c3db183091cdbffd023d86e27c93b490",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/ubuntu1804/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_ubuntu1804_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "63542b336dfef39a92f0f5cac9d97a192eef68c0f80df30556ba464e8826ea6d",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/ubuntu2004-arm64/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_ubuntu2004_arm64_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "d1df4ee545cfc324d30e07386beeb27d34b2424303898c9f0f3c6e45ca8a8650",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/ubuntu2004/11bae3c145a48dd7be9ee8aa44e5591783f787aa/bazel_v4_toolchain_builder_ubuntu2004_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
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
