# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_PATCH_BUILD_ID = "acdc1709a01126c67dbd4f0f37491e171f64ec29"
TOOLCHAIN_PATCH_BUILD_DATE = "24_10_11_23_01_02"
TOOLCHAIN_MAP_V4 = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "405b63f369fdcc9767314a9ae9d488a4f8ad1eaecd181f5f95fc090d6b218614",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_amazon2023_arm64_acdc1709a01126c67dbd4f0f37491e171f64ec29_24_10_11_23_01_02.tar.gz",
    },
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "48f35037f7e9f49c3c9885ea57d9cc59f27550a6e0dfb226e8f2396373b1657b",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_amazon2023_acdc1709a01126c67dbd4f0f37491e171f64ec29_24_10_11_23_01_02.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "1db4c8ef6b30b3a72f2c929247dcffed3a99356099b84c16b8b20efdf99e00d6",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_amazon2_arm64_acdc1709a01126c67dbd4f0f37491e171f64ec29_24_10_11_23_01_02.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "e2bf59dacb789bd3ed708bafb7bf4e432f611f19d6b888340e3b73eee6949b31",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_amazon2_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "debian10_x86_64": {
        "platform_name": "debian10",
        "sha": "9fd2e74e160699de706aac08bd94aefa7539e835693773c56ff6019847df7cd7",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_debian10_acdc1709a01126c67dbd4f0f37491e171f64ec29_24_10_11_23_01_02.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "d34bba732f9a4846d6eb27bb1c2f00586fb341a249a12fbb0216268b3b2e69ab",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_debian12_acdc1709a01126c67dbd4f0f37491e171f64ec29_24_10_11_23_01_02.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "5b56277cc3a9820fe09774d6a2a6c861b306d7cd8a7bca55848193e249909033",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_rhel82_arm64_acdc1709a01126c67dbd4f0f37491e171f64ec29_24_10_11_23_01_02.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "b62574a447e0e5120475c7ef0ec0e821548a32a103999ed350b05adcc9073b55",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_rhel81_ppc64le_acdc1709a01126c67dbd4f0f37491e171f64ec29_24_10_11_23_01_02.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "5d4f93968672f324e532b62eb9345351b407a866650d824eee79d295d6613606",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_rhel80_zseries_acdc1709a01126c67dbd4f0f37491e171f64ec29_24_10_11_23_01_02.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "4b3406838c9adbb47d308ba4028ca88948f9bab336eef591f7b8395781ab5cd5",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_rhel80_acdc1709a01126c67dbd4f0f37491e171f64ec29_24_10_11_23_01_02.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "7c6ce149660197f4b63c50404e0c8604285299f6fcf67c1f9bae1de0bad606e1",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_rhel90_arm64_acdc1709a01126c67dbd4f0f37491e171f64ec29_24_10_11_23_01_02.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "ba2cf2efd68805eec11580e0276f2f4bde6a94735b0375cae901bcb7099daebe",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_rhel90_acdc1709a01126c67dbd4f0f37491e171f64ec29_24_10_11_23_01_02.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "f7ac487d2ace8d4b8793de712f9e7d7cf3bec35bd82da39b30f42db76f59b919",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_suse15_acdc1709a01126c67dbd4f0f37491e171f64ec29_24_10_11_23_01_02.tar.gz",
    },
    "ubuntu18_x86_64": {
        "platform_name": "ubuntu1804",
        "sha": "526b64a04731275ae52b9d8b062efa20c3db183091cdbffd023d86e27c93b490",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_ubuntu1804_11bae3c145a48dd7be9ee8aa44e5591783f787aa_24_01_09_16_10_07.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "1f0bee3bde3611d1f2a5f10ea521f333f508d0f3fbd3f4675c77bf0aed1037ec",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_ubuntu2004_arm64_acdc1709a01126c67dbd4f0f37491e171f64ec29_24_10_11_23_01_02.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "1dc69e016569ddefa925ee09961e0e466ed80985b767e0f634c0c80286573952",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_ubuntu2004_acdc1709a01126c67dbd4f0f37491e171f64ec29_24_10_11_23_01_02.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "536829b77db6b40fc4b29f848e0b31715e61fe7bfeca0a58575e52bdf4ec0b2b",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_ubuntu2204_arm64_acdc1709a01126c67dbd4f0f37491e171f64ec29_24_10_11_23_01_02.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "e7b305c973b69268983431e1b11ddf1f2fbf38715ede2c415b0cf9a4fcdc0615",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_ubuntu2204_acdc1709a01126c67dbd4f0f37491e171f64ec29_24_10_11_23_01_02.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "72e8188295d3296e0361534762e1f4226242b1c65001628cff1f9aac60d94a94",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_ubuntu2404_arm64_acdc1709a01126c67dbd4f0f37491e171f64ec29_24_10_11_23_01_02.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "d08520df222f81253eb9b9802fd8c7acd02dd0d60f08776b45ea47401754f947",
        "url": "https://mdb-build-public.s3.us-east-1.amazonaws.com/toolchains/bazel_v4_toolchain_builder_ubuntu2404_acdc1709a01126c67dbd4f0f37491e171f64ec29_24_10_11_23_01_02.tar.gz",
    },
}
