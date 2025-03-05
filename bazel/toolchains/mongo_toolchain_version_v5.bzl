# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_PATCH_BUILD_ID = "fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b"
TOOLCHAIN_PATCH_BUILD_DATE = "25_03_01_20_12_20"
TOOLCHAIN_MAP_V5 = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "6f7c3a116b1b3a79ffb367fe405b1311f32afc9c50dada56679e62fbf5e41e56",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_amazon2023_arm64_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "8cf8bc6f3ae4bbf47d013aa4a7ee2b8a22d0e375d455e7d187504d8283974524",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_amazon2023_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "5391e57423a4e5e919ba30be7a55773a0e25506211de06da2fe856d48c9a0760",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_amazon2_arm64_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "85327a2b76193c50f36808aa3f6e0c925761b67528daf8b0b94d4b24b93ce673",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_amazon2_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "33f2077f8bf088bca73a2fd4fe0cb5b7066c14d4be7b45fa12e2e37d032f5c2d",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_debian12_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "1aa3630efe0db3c842c0a99d8d89cc6a384529d532fe63278a7763dcec07b941",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel82_arm64_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "fa5ef3908902b73388a5a831db990aac6346c9650b620960d21629ea6debdf7a",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel81_ppc64le_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "97e4a314a943ce63888a880ebbcc7eb1539ec55342a8d1739efa8cc2a70a5cf6",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel80_zseries_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "34dd0f3f4052863e14c6b84aa6fd5b0c246712a148b606f1cc96f54906f69ecd",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel80_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "8755318a450af127c9eba56908b64f0db02cad7c7eeed34cbf8dec0b9a61e97d",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel90_arm64_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
    "rhel9_ppc64le": {
        "platform_name": "rhel90-ppc64le",
        "sha": "a7f5abd3e14e4cbd810a4b920cbd8810bb39bec2e8976066586bee41c275bd79",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel90_ppc64le_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
    "rhel9_s390x": {
        "platform_name": "rhel90-zseries",
        "sha": "bc10553a180b99c3766761f9c0363210e57a8e460985ad5610b99e40eb5c5428",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel90_zseries_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "0e25842f6ccfbd54c8f5c978ae1d296741107dfc1d6e870895c568b5afcfd2c4",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_rhel90_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "70f648078985f7b74b95cd6d52fc92db3c29e300c6505d49ac126361bdfcb177",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_suse15_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "aaa4feb1e74c919f02f98aab8e1e44fc7d0c505d84de0d92441a11d6df4b292c",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2004_arm64_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "b0d238faf051d54d816c4c0da2d6532f190e35f42c87bcfedc0fa9790ace2a06",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2004_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "d92b0ee53d6ec9f2392a83d0426e79132333b433cb33ec9619fb17d675b4ebf8",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2204_arm64_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "e2349d64027154edde2a4bd12818eeeb2864900475bec8d55f2b793c37457eb6",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2204_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "fce2f0f45d55186b057c787ef0333c06843d3944156663d52166de220fb3684e",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2404_arm64_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "ed71ef30fa4168f44ada182ae1e225e41ce5ba5be87d2832e8f827292a487dcc",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v5_toolchain_builder_ubuntu2404_fb8eb86be2e5ed5f91e7b4108f38f3bd36ddba4b_25_03_01_20_12_20.tar.gz",
    },
}
