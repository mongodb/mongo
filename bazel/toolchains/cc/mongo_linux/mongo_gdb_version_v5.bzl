# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_ID = "84bea98f485ef8c1af3f0612c56423fac4ea6256"
TOOLCHAIN_MAP_V5 = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "a3115f43372601bbd1ef3e812fedcc521b12194fcde317c6c499ba8a7084ed49",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2023-arm64-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "ec74688444aaf319356f3442b7f6d770a3d9503843a3179f5c9961c86aaac913",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2023-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "20037a9d237c3ea00bfa983dcc5e007b84c107060c1152606d51e68ccb14ef39",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2-arm64-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "335e03bb5edd0b906ad6145fb41cfde6880b4b2d8cf84e76caa4c355c66ca489",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "debian10_x86_64": {
        "platform_name": "debian10",
        "sha": "3efb1137d0b3aba0b29eddbdad19c39b203187f58128a5b93269e03a08e1fba3",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-debian10-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "94803cc5c82d24e1c991327dc7dc742dac1f79bbb9a2509b6e0b967134ef6923",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-debian12-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "93a5af2e9f5f0a0116516ad160ee96234c0341bc5a7ce56bfb192a512dbd6375",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel82-arm64-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "4cbce4af6eee5014e87107b4996834b7c3d3469b61ecc5aaf16b6a4770711c1b",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel81-ppc64le-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "fbec5ff54b063c72121f9932c05148c8a0f67996ba033792d5c37d7884cb3103",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel80-zseries-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "17522c32545cef0c1c0d9e3409dc576c2fc277ff4dd0a1190ff89435209a587e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel80-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "5d9addaa6abc401e3f033b9709bc9293f358eacd62a9e66a6c2f21878a3ca60f",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-arm64-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "rhel9_ppc64le": {
        "platform_name": "rhel90-ppc64le",
        "sha": "49da44be2830b3bfa2153b4c804a4680edd66fdc387259fd12895a43f751586e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-ppc64le-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "rhel9_s390x": {
        "platform_name": "rhel90-zseries",
        "sha": "e6383442735bae668b024aa03d64d8396a01975ddb88989f40eb9b70c685890e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-zseries-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "d04680a41b5b804a22f5880bf5458b3c13e0fca2863afc0529a5bf26ab74938a",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "9e9491110957e301c2612f4550975319f9b13b8544ba1871d413f8924102a28e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-suse15-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "4bfc2bc218a4406cff8c19e07358c70108e304f7cddf5022ad1714e11cadd7ab",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2004-arm64-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "49aec1fc396677a50d17fdbeff9c631816e8bf61658759d970f4a36f8b59f802",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2004-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "7a970159806ce10d882a002a1c92ecbdeaa249ec640eecd50885fcd892965376",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2204-arm64-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "063334f99a9dbb99c5574d705ae2971815109083246348819d310650f70372eb",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2204-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "2c1e179e0e7a277c01c262886cd58b2a67b9ca628858177aa87589856301e56e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2404-arm64-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "7d64ec6cb9b58e0ec8c30547c0f405ae9aba76170143e850ef6211b530a24fa5",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2404-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
}
