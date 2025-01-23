# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_PATCH_BUILD_ID = "acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a"
TOOLCHAIN_MAP_V5 = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "29273b405e6781dd4f8ba95a12d90117cd21318605b6a7cebe3a6c66fada4dd6",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/amazon2023-arm64/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_amazon2023_arm64_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "cb84ff08cc96777b52d08235e392ac13392ec4e5e40efb7ec3046962a8e9a7e7",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/amazon2023/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_amazon2023_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "a9f834a307dd2a660c56abb49935d2a337459f669156b19d5f82625a287bf432",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/amazon2-arm64/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_amazon2_arm64_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "62320dca3d40e14a7d881215017abeb452a5ff6d35afe0f56dbe8810a69102d1",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/amazon2/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_amazon2_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "714859ea3722a989042e373ec7b04c59b397491a869d798ff71e65abdb74c490",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/debian12/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_debian12_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "f9dddb4ce3a1df1845e7d31002ede666028ecdd07c31b2fde938bce9bcd921d2",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel82-arm64/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_rhel82_arm64_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "1fc3f4314d6a3495e8de4cd1a79ebd0d69938ad06ebf9b31eb283cd6fa667362",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel81-ppc64le/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_rhel81_ppc64le_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "a84d7428f4d5e00d2370617123be6f5a7419a0b642c00825d332c20a20439317",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel80-zseries/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_rhel80_zseries_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "b70bcc4066ad3b2d6c929133bbe778c96da0b8bddd3702fc7e31567ce933c819",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel80/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_rhel80_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "fb4b10f6ae2afc44342fa0264a7762b4ea2e32a6fb44b00f8bbd60c490e95b1c",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel90-arm64/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_rhel90_arm64_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
    "rhel9_ppc64le": {
        "platform_name": "rhel90-ppc64le",
        "sha": "df21071fdb05c31561a7cef4bdf71784d0316369a82598d5059e3818080128a6",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel90-ppc64le/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_rhel90_ppc64le_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
    "rhel9_s390x": {
        "platform_name": "rhel90-zseries",
        "sha": "ee10bd57f428e8dc36e42b132744cab96d1bb43587c45b6f81be8eef9d3d3246",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel90-zseries/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_rhel90_zseries_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "a81b5143a96393420f0f05f0fbf019486ae56c6389be70b5e25a7cb00a834c4c",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/rhel90/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_rhel90_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "24b8988dad7a1639d04475318abe6fc7c4fa93e1aabc0159763d2ca0f77386b3",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/suse15/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_suse15_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "77a821c26e1392da3c13ab4faef56999b125cdd0300a63ecf8cbfff267265c28",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/ubuntu2004-arm64/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_ubuntu2004_arm64_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "2be31b365f79b4abd012cc6aa0aece268be704278ae27c04f38dcf286068acc1",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/ubuntu2004/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_ubuntu2004_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "e12e043c374ab18ad736e20910fb1c1d7916eba4220bc78874ad4148f84562cc",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/ubuntu2204-arm64/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_ubuntu2204_arm64_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "8bd3a44b76e31fa2e7e28b42f6352d9fd712573d92f21240f2be44e7510cab4a",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/ubuntu2204/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_ubuntu2204_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "520375e146beea6d65f350c9f1d553e14534d5b2f56abfe6af6e0b9ce1ad7253",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/ubuntu2404-arm64/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_ubuntu2404_arm64_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "c4db62d5a68eaad907a1475de0033899abccd4534b34afc20c085c43451ee8e2",
        "url": "https://mciuploads.s3.amazonaws.com/toolchain-builder/ubuntu2404/acdc1709a01126c67dbd4f0f37491e171f64ec29/bazel_v5_toolchain_builder_ubuntu2404_patch_acdc1709a01126c67dbd4f0f37491e171f64ec29_67912070317bec0007ec5b3a_25_01_22_16_45_39.tar.gz",
    },
}
