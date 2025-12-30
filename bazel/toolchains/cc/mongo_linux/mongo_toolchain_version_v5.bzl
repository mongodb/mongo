# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_ID = "8c3aa505608c8462ca46fd42f5323c4e15c21d75"
TOOLCHAIN_MAP_V5 = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "fecda1d83bc31cb795adaa9bd5871636e885db05140fcf62b428dddc1041df88",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-amazon2023-arm64-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "d45bc4dfc788a50c106b907a6d991bc28b8e2ae6d218b17e7f415b7f48cf6271",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-amazon2023-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "a1fb0d624af842fb013b7e5c53ecea8d59ff30b79e9a7d24755d77cb198e612c",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-amazon2-arm64-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "2c0780cb53d38b83ee50da29e7a4b555fe333bae8f4f47596b07c5be35e06e8a",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-amazon2-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "debian10_x86_64": {
        "platform_name": "debian10",
        "sha": "062ab3cd8d4aca9a041d07efe30bdbd14949144d6245636099423b8df1b887f6",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-debian10-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "b515fb680ba39215cf62e8b1f88da82e39f7e64a2ca13fe3d74962324b9a7aab",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-debian12-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "d99de8106858063d26c20d064c581d23af477b016367af51c305f2a870655e36",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel82-arm64-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "cf82df09e0dcd70267fc792b90d9b151186cf5528c5a00e87e5e00cf2916817b",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel81-ppc64le-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "3a9e86c457fcd3b7bbdf67848dfe1b1e9a9e1243a86fb86c3d30cb1ad3d73fe9",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel80-zseries-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "c703380313e37cee03fb479f867d91f2a60221e2880b2781d75d81ea49f428de",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel80-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "4c0edcbad6a3c5e6e6ddba41699faa9634e8c4e80e28f9e4248efb935d751414",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel90-arm64-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "rhel9_ppc64le": {
        "platform_name": "rhel90-ppc64le",
        "sha": "198b0b0e75daf25fae610c460d60a7112ca5dbcb86d3cc0d820da75d073866e1",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel90-ppc64le-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "rhel9_s390x": {
        "platform_name": "rhel90-zseries",
        "sha": "60db7b6a8cb351aebe36665722e4624ea282429e7d1c1e704bde96006159cd77",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel90-zseries-d0c2e5e88069c3eb0f7bcd61bd317e288a3c917a.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "8f16b5dfbf59945a6d130182826b3ef0149a3d12690a60a075023b15b98c980c",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel90-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "1971b554d55a38ce39d408f17cbc0b5e8b43860e83089578e4d8ae4b2534d52d",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-suse15-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "ubuntu18_x86_64": {
        "platform_name": "ubuntu1804",
        "sha": "988ec16c606e3c735ca4791b5ec1c5d19dd6297d90982fae4533aae674a1ccd7",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu1804-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "0d1d907d48dca34dc3fd3b06b1651bed63e1f6e319f91b9a8a1047172ad201f6",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2004-arm64-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "be14f48eb83610b5983c263e9c2973b97931a0a9ea23e537b12e7b4ca76f1eb7",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2004-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "78a4f6e3ccf0783a0cffb53d0ec3c258dea47ada918d3a7845c3b4e6092fbd8d",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2204-arm64-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "580bae81351cca8b41f61f5fd9a02e3a41550fbc3c9357d511cbc3db9b325f4e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2204-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "92ee2d0ab6882209937e5ffe5943fe0a2334ec8689e1a0bf7914c9085c0dcd45",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2404-arm64-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "ff9e142c9cc324306f1c08154d2cdda9a1d067f89e967fd2f4c4d72efe1a33bc",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2404-8c3aa505608c8462ca46fd42f5323c4e15c21d75.tar.gz",
    },
}
