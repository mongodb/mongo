# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_PATCH_BUILD_ID = "79c9b62fe59b85252bd716333ebea111b4d03a12"
TOOLCHAIN_PATCH_BUILD_DATE = "25_03_13_03_06_03"
TOOLCHAIN_MAP_V5 = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "f3ebe3ea5e3ca9f72028eaccbb7bc1c304be62327dc2ccbe8fca83e788c98073",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_amazon2023_arm64_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "e3e8175c9995aa40f1f950e8c351b5e3fc51bccd081f31747339a949ddda3224",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_amazon2023_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "fa151e76ffa90cbcb75895b266371df593832b80234989bf1806bc83011207b8",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_amazon2_arm64_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "8dcc6d36a882b183cf38d2e3c4938a9c55b6b534aaec64357882994544b70f5d",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_amazon2_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "93cafde7630605fac075d54dd6b5a07895e956a0ea5a3296304f0a94c21a0c0d",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_debian12_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "55b23535be078aeea6225bf113dcb433f349d0f70803520e3e1a98d96408fa44",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel82_arm64_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "eccf7db4d39149aca7b1fbcd2ca1de466815e47e4d100827aebe05df06478536",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel81_ppc64le_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "c59c7e5774f98c61a8868297e2b040b44a4977003d10dc8839cf770622644572",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel80_zseries_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "aae5fc76bae6828e241108be7ce6897d53f606ef86134c1725cd5d977899e9ab",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel80_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "2c7572de7c4d5fb901aa3f64c19a787e378332cb49dd0a70ada88aebedf8c838",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel90_arm64_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
    "rhel9_ppc64le": {
        "platform_name": "rhel90-ppc64le",
        "sha": "afbf592866f829725cf20b9169a9b378693697e5a23c19ee7775a00af978fd6c",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel90_ppc64le_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
    "rhel9_s390x": {
        "platform_name": "rhel90-zseries",
        "sha": "3fefe93e7f5b33fd1b98c7cb823928cea5b733fa4eebf9af4c7d785be23b9503",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel90_zseries_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "e8934a8a9ef56a3a33393bc0e363d318c335e74307ad7608b835489653dfaf90",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel90_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "4d036974dc1d78c8c358624409b3ccc3b2ba87badfc0ccc3edca6a9ac77eae18",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_suse15_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "51007acf258f75ca5bf2c595e1825b1bc16222e4d487e65653629e321ccdd9c2",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2004_arm64_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "10855a1e8f030f5645491503f2ab2ecb0368124ff2bdd2992e6acb900be02375",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2004_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "dff458bb44ff5c52a27bdc09272aa2cd93772fc6fdbf0a302705acb255b71f15",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2204_arm64_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "4386dfb6a879c93405ad39d773d0c2b83dc707138cc29fffb17adc985d9c7e37",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2204_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "9a6a57a6c6380d7b5c2d33afae6a9741cd36ae5fd1348b8bf9f765766b14c001",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2404_arm64_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "06e7311ea4e23b8d2e02e21371cec035222e285bda71b673c9e938b27ee980f1",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2404_79c9b62fe59b85252bd716333ebea111b4d03a12_25_03_13_03_06_03.tar.gz",
    },
}
