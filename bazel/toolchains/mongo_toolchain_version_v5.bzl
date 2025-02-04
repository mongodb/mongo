# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_PATCH_BUILD_ID = "a4fa792a94d75177fd3f00eb3ab7516e5de15485"
TOOLCHAIN_PATCH_BUILD_DATE = "25_01_31_22_50_42"
TOOLCHAIN_MAP_V5 = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "bb75abafa80e32972389ade89d7d536b538d6736749669439761b9870790821a",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_amazon2023_arm64_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "175d43aadc29a367c98765d1ca0700d1c171935f96fdd857924135fc04256c21",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_amazon2023_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "bcb239293d09ff35c0e070194c143c21fa18bd28bb5c1a11a576afa05370a302",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_amazon2_arm64_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "7c948b6d9b8d524f3a86a470e39c82af47f3223617e0724b56e819d153550fa1",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_amazon2_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "29f9c63fcf5ccc75b45de86db70a105f9c8bc61dc70906311dd435e4f6d2b128",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_debian12_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "a54164b15eed7fa0fe5f48f3e426a336ce2199b0ee3057347a507127c9b88568",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel82_arm64_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "d3b6bb2b60ececa7f2a72222e31a38c937d2f35ca64d7b09ae4285b42ef78d50",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel81_ppc64le_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "0c3799aeef57a91a256c392ff51d9f0eea21aed43bc5e737e27a8b60269a0b78",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel80_zseries_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "ed471dbb7c6396efa21b57290844e2c96ebe05da9031f3c022f9c75fcedf67f2",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel80_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "6f7783f496a60d0e17716f85e8da3ede7cfce7a1272b54286e62fc09b334f3ac",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel90_arm64_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
    "rhel9_ppc64le": {
        "platform_name": "rhel90-ppc64le",
        "sha": "c607a733269a9d2623c16647905ea6e0521eba7e149beef2ec22b6ba2f0d6745",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel90_ppc64le_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
    "rhel9_s390x": {
        "platform_name": "rhel90-zseries",
        "sha": "1f7cd56ce9fc17bff2befaa8dd52ddb9ce8d8227e7f4b125c277bcbd64bfe680",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel90_zseries_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "e4298077976a00ff37fc2f799e221f288fcfb8e34fc3859e8e1475e149f0b435",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel90_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "222117b0461f00f996103715be663ccb135895c129d8fe5c73bc54dbb2367a60",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_suse15_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "279e96e239ad5501e3581a38f321c8a2bd6266e2b76a5358eb730f5fed39dc90",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2004_arm64_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "a21c35281cc066646e6408db1b606179c745198d133f87a400c34fc0ca3ed10c",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2004_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "cbb200b903ceed788326cf8fd28a93726f94714502ab88a5f922693afcda9c3a",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2204_arm64_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "f8c88f027a9a2dba68af92f20b0f8b4d2b8f23ff833b6a7278e2705702b11668",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2204_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "f09c2c39e2f6f6b7c421625ac265e685bef3e5445a0814d38d77a8b72925c642",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2404_arm64_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "8a8c1ee92206b678d1101292879b998e7540fe168b56fe1eb3fb19c678cebcd9",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2404_a4fa792a94d75177fd3f00eb3ab7516e5de15485_25_01_31_22_50_42.tar.gz",
    },
}
