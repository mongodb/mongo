# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_PATCH_BUILD_ID = "5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af"
TOOLCHAIN_PATCH_BUILD_DATE = "25_02_06_22_19_24"
TOOLCHAIN_MAP_V5 = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "f1d947432dc269b420885fc89d7c2ba577b332f023953df7fb470eb7613352b1",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_amazon2023_arm64_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "d18801685f4f9a34eb492e4f5a91633e0b11fab60ce15974acbb8e919add6005",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_amazon2023_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "3409ddbd917f1bec144f651c1f58eff17c26a170d7a430582acea84ac57dda88",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_amazon2_arm64_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "61a174efe6c208495057aa3430320a8c799aee530e06ab4c1276039d20b0992e",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_amazon2_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "f0b94f1383700ef9a19706e8e20a22c765f07129d5ff9d3da4e87a9bd9fd1748",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_debian12_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "00ef70d1f0343744e81fad022a1017634ac6765129e885a4a9ef2d51c984d99b",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel82_arm64_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "c28140d05f65a9341a275c746c5f12c6e41e65f41d85a71b49a6d8bf8abbc8fb",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel81_ppc64le_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "9a0c75930c61407255983d76c4a0ca87f20c748ae7ae35dfd95757404ba6e7cc",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel80_zseries_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "1770f3b23516a98e7191ca7585f4717577f08faad8d1813e4986d0ca936260dc",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel80_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "507c18df5fb3d87890b0ad7d0e43c6f1767202a851e4d07602092ffec325d8b4",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel90_arm64_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
    "rhel9_ppc64le": {
        "platform_name": "rhel90-ppc64le",
        "sha": "b8d59d347c84621e6dbbb5da2ecc9385c42151afdad5f45eafe12ef71179c395",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel90_ppc64le_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
    "rhel9_s390x": {
        "platform_name": "rhel90-zseries",
        "sha": "2d220ae560c47ec5466bc35259de0713ab7f68f16708599570fe0baf7b63a8bb",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel90_zseries_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "afd0e00984181bf0ed091f62e987b9cafadf72ef900ff5ae1fdc69dc269a27a6",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel90_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "46c7adaebae450aa5e6b9f939270dc91e0099772ea28a9eec9b0ef189bff8d7a",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_suse15_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "1d960fb351a483e1067d2f4a125b53e836db0619b8c80c5f9153e2a365aa066d",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2004_arm64_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "61a6f7109dbf11c51472464cf90b50df5741553ffae3980f9ed90f100aa65c2c",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2004_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "98226afd89ff1f92e2980b1e3bbf81852a57b5897fc9f512c443fe708a98605b",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2204_arm64_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "c616381eaf39d7d47526f0b226e2b3d97c3d59971f45b4933999a78587237bed",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2204_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "70dcfe6c600a544dacecc1d0eb08cac2cc09ab88bddfe311c7b93507e6235bd1",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2404_arm64_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "a8701d911dc2fa8a284fcaae9ff791ef6a74e439e9d0287b251719e68bd01b3e",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2404_5c38b5f1f5fd9eb734d4d8da79892ff2b4e2e5af_25_02_06_22_19_24.tar.gz",
    },
}
