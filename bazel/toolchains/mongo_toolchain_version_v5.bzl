# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_PATCH_BUILD_ID = "acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c"
TOOLCHAIN_PATCH_BUILD_DATE = "25_01_14_17_06_29"
TOOLCHAIN_MAP_V5 = {
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "b1dd7d1ac612aa4fc21fa9b853572843a9e8d894ccc87c13136c65c14a6dfecf",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/amazon2023/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_amazon2023_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c_25_01_14_17_06_29.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "166e880cc7d639d304488d31736eb3338fa9c6b3a3848880a344235b57366e18",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/amazon2-arm64/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_amazon2_arm64_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c_25_01_14_17_06_29.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "10fdf726b2a329ef682c820b0d26a0df054e91ee9ccbc592873064adc0a89fe9",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/amazon2/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_amazon2_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c_25_01_14_17_06_29.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "760c4926adacf613aeea130f647ef332f090ab5610a22f141a6ce810c1eedee6",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/debian12/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_debian12_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c_25_01_14_17_06_29.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "cc8eaeca118933e2d9bb623433b2b9e5b32109250c7b8b7467854ccb9dcc8fbe",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel82-arm64/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_rhel82_arm64_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c_25_01_14_17_06_29.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "e0cc3d03f8ae89af7fa74de7ee7a2af3fac8ca399d5064fee9dcca2797ff43a0",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel81-ppc64le/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_rhel81_ppc64le_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c_25_01_14_17_06_29.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "45ce8b545cc781864936a74ba899baa3d2c62fa50d6ee3fdc66eb26449538fdf",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel80-zseries/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_rhel80_zseries_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c_25_01_14_17_06_29.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "acb9d75cdc0772c671121edf7ac27e9b4a75b985cc3d967ff9e0f3cb29e0dff3",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel80/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_rhel80_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c_25_01_14_17_06_29.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "c0c075ca8d1f0f9f955fd93f92c01b116a796f91489cf9c58ca2c74722ab2108",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel90-arm64/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_rhel90_arm64_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c_25_01_14_17_06_29.tar.gz",
    },
    "rhel9_ppc64le": {
        "platform_name": "rhel90-ppc64le",
        "sha": "b63bbff985680e44b65508544f920348d98046eb0b371f550e4892c35d3c41c2",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel90-ppc64le/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_rhel90_ppc64le_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c_25_01_14_17_06_29.tar.gz",
    },
    "rhel9_s390x": {
        "platform_name": "rhel90-zseries",
        "sha": "5c40b7f4d69d057488f67765b98504aac4208f86af81280da24d24e3ac8749ef",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel90-zseries/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_rhel90_zseries_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c_25_01_14_17_06_29.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "41d4b53b99e2283ff40f3070edbb676576fb3ae8a78f1fdb8f081f97270f898b",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel90/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_rhel90_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c_25_01_14_17_06_29.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "c3989472373db7ca5ea19fbcfafe481edfbb7dc4f9b905b0f0d4dc252eac6830",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/suse15/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_suse15_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c_25_01_14_17_06_29.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "d48314b1a5681c579c318409bcc46476ce7e3dd3042ad7ae39d8d53cfe146468",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/ubuntu2004-arm64/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_ubuntu2004_arm64_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c_25_01_14_17_06_29.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "4aa9932de26fb052c0a8adfe68534088144f2722a548a4157e783f1ae5daad2f",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/ubuntu2004/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_ubuntu2004_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c_25_01_14_17_06_29.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "08b79cf7b2df2120531176945f69db8dbf32df86fde6bd5125fcb69d9077e7e1",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/ubuntu2204-arm64/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_ubuntu2204_arm64_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c_25_01_14_17_06_29.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "d0cf9d529faabc8861f8b677600d2f5ec6e83a2ddb9f91b08e8d1fcfbace3156",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/ubuntu2204/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_ubuntu2204_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c_25_01_14_17_06_29.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "9f3d0257c03afef4bf5f9bfbf4e1d5d89f5dbadf27f6933ba7307b361571632b",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/ubuntu2404-arm64/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_ubuntu2404_arm64_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c_25_01_14_17_06_29.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "fd316763f08c919c0b5f8efb56350b5c40324cd49f12cb0786c4af91345b4468",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/ubuntu2404/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_ubuntu2404_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_678697d84ad33b00070a840c_25_01_14_17_06_29.tar.gz",
    },
}
