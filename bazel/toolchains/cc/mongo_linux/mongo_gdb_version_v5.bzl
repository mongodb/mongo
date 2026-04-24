# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_ID = "7553752a78d885cc2f60c725bfb5cd66e4c4e02b"
TOOLCHAIN_MAP_V5 = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "a9c260b5dfabc88c84afe83f32ba085269b40e5b9c8b84bc935053b55b6ac4f0",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2023-arm64-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "b59b16d53b6b5127aa7419d71198bb9bc1af0896ca3ed98c5f12997da7b89804",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2023-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "356d585508ff8a846eb572ac81b8d44d57035442eca3be8bad429747167c11f4",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2-arm64-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "6e4137f27dfe8ad529464b45aaa79e28d5931ee5ae32614dbfa2fae4c2c3b142",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "debian10_x86_64": {
        "platform_name": "debian10",
        "sha": "81f19d34c452e0b84820aa0e5015c46486bea86b108cd5b90121f8ed7a25099f",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-debian10-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "b9fea161c61b97e3f97b64da7e085020f980dd5e9cfd81a35ca5dc271e3faf87",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-debian12-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel10_aarch64": {
        "platform_name": "rhel10-arm64",
        "sha": "7575be150d1d7e13d9a80cb3a70329fb9cec045bcdc745c6e4c94ea6fd2f4336",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel10-arm64-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel10_x86_64": {
        "platform_name": "rhel10",
        "sha": "679f501f0c50d71dcd35c9d420de6273e30b63cdc7e7b04ca86ea3e9256a9485",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel10-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "5569fb6245f9b5b5bcaad4ac1d33688b919df4ee8a84ee8aa38d8de73c58a21c",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel82-arm64-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "b8a29cd514cd3bd3da1ac4d4b6f5933129e356bca52bc978d952227afdad9885",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel81-ppc64le-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "1c78442c942c451895c72ff5071d762ed021a50f403eb3d5e14494515b93794e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel80-zseries-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "7ba715e168a1b48f6bc551f02d4b993aa7472ecd84b85c4b072122cdea0b084b",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel80-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "940412515804afb06ae900b4c5bca853184311bbdae6c2afe2c307ac23acde0c",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-arm64-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel9_ppc64le": {
        "platform_name": "rhel90-ppc64le",
        "sha": "76c1a20b579f37bbbce25580ab9359e7e0d4f1300204f735e9ff2c5c177451ec",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-ppc64le-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel9_s390x": {
        "platform_name": "rhel90-zseries",
        "sha": "fb26505e05567d2e9f3b0b2d2e0ab7b97040f39f4cb2747ee128faa39fe5e161",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-zseries-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "138dbf0c3a80e6aca597a17832b48a6f8d4b5a7c410100941e39451de1a55d88",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "7a7f31b176113b8057a24efb03952b69ad40c25a6745f43182a4a8511b0ed72f",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-suse15-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "ubuntu18_x86_64": {
        "platform_name": "ubuntu1804",
        "sha": "d29c585e14b48a4598f1d1a1ee93425c050746967c025f7c0d250c5857845a91",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu1804-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "b7242425f01ea70620f1db490d74cc292dbb0cd1310dc6229a884e989ff44190",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2004-arm64-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "ac1012a4d52dcfc0273353acdf375800d2ac42ded2efabfc8c47dbd30c4fac12",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2004-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "4d68bd69ff3bc57aa9f870ba9cce38afee7d65713e4a0eae09845e5fdaee6821",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2204-arm64-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "ff802afe26798075cc853fb0c68724472782ebe32f250042e9c2313448b68497",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2204-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "db58f1a51bdda3bb06f6714104052abceaf229c62ab01359a24bd5f78a9cf1a5",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2404-arm64-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "f111ae80389ae91231596eea48af30bb910cbf32e292569767aa003c6e8cb3a7",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2404-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
}
