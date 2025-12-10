# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_ID = "8c3aa505608c8462ca46fd42f5323c4e15c21d75"
TOOLCHAIN_MAP_V5 = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "accc96870419f3d759ddd269b1614a19710fb7cf5c4d9883f00b68933d288629",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2023-arm64-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "b8c9641021e4106916655b34f336ad28dbd375e33878fbfe2cd19d417df9c628",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2023-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "bc23afcae4e388f84fda05b4ae5c3f2661adb6ca5cc57cb519f1db5d9bf0fbf8",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2-arm64-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "9759e83ee8ca0659a2bea2d235f0b4861496a352a50103fee697961345dc0f36",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "debian10_x86_64": {
        "platform_name": "debian10",
        "sha": "1d6c83baf52dcbd1b50cb9f58432225b354b77a57c035dab14d21792c75c3b1a",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-debian10-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "2b2a9598a607e28daae5a25f38b5f662d118c6d7923ecb4eb69fd5f0491d8b70",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-debian12-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "a730c103c9c4a72a4efb3a77459ce6a0697b19a3638eb2471a648ea555dbc24f",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel82-arm64-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "006c5e1abfe889b77c3b9459ac4b0258604e9c2f9ffa9960fc5a9e34926c3929",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel81-ppc64le-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "f4bb09a8a71e7a9011f4ed1b4f5c3857c4439361a209e22662a61c5b4c29fb11",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel80-zseries-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "675677cf683ac98cd8774ea07ab12d420842cd2333860e87326e59d5d6666d33",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel80-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "21ec18907e8df5e535f9cf6844dcddc6ee5fbe56e7faf58d191b766a7211f2cd",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-arm64-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "rhel9_ppc64le": {
        "platform_name": "rhel90-ppc64le",
        "sha": "ea2140fc64057a5aae361cde8a38a71257257db7987c950f5ecf31fad909c563",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-ppc64le-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "rhel9_s390x": {
        "platform_name": "rhel90-zseries",
        "sha": "e6383442735bae668b024aa03d64d8396a01975ddb88989f40eb9b70c685890e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-zseries-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "7dd01b536c8db4d435741ca6637838af403944d4bf073e5a494bf80b889ea101",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "eb3ab6840b45b144696e88b2f2e91d9aaa5a7e2245971ab568d0cf38242e17ef",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-suse15-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "ubuntu18_x86_64": {
        "platform_name": "ubuntu1804",
        "sha": "ef27b2067da7308996c7ae81fa81d841f7f5b249b34c596d7f747e264946c441",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu1804-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "d29af42bd88d6f2b6b10f42c09d1ee7029c77d98c7fa83aba24c76ffb91f1887",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2004-arm64-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "1beef847f1ae215d65c1c75bc5be1f7f719a4cc63adbc744b663c61eaee68a23",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2004-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "ffa865771a75b6201d453198356e052bc7fc1171010c72804888ba105ab72f69",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2204-arm64-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "b30873af65e14a51a1de24b1baf38bd9093a88a8cf83c515f5a3091c949e0d77",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2204-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "2728f48b31cadd6e41ba35e20b3524325ecec9554c9064eefd176e3bba2f7411",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2404-arm64-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "8ccf9da391302d2ae81bf07a2d0991ec7b42b38a89654e44cf68b937306b8678",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2404-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
}
