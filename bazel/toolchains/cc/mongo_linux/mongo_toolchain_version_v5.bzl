# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_ID = "7553752a78d885cc2f60c725bfb5cd66e4c4e02b"
TOOLCHAIN_MAP_V5 = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "1bfb3fc927b5192dc1ca6cda27e8f59475457dc2f2e5612cc347ceec506ddbde",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-amazon2023-arm64-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "10f396b44ae5c5d59ed2333ff2ae8324e6e414101ea479a0eb2984781774375e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-amazon2023-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "106b74f06cf8539bcf0873a5f00391ea75703b32730b2385e564f12bc22b44e4",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-amazon2-arm64-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "898097ebfaf7a5114513dc354587326434631ae8866449328fa7c8aa0ed494bd",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-amazon2-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "debian10_x86_64": {
        "platform_name": "debian10",
        "sha": "119e234e4465ff789583b93e7a84e775e2b63da7150ec4bf26c813ce69fe515d",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-debian10-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "94b13eb9175567b2db7f9a3ee0abb1a1b619534a9bef0c7a6a21120c9ab5f926",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-debian12-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel10_aarch64": {
        "platform_name": "rhel10-arm64",
        "sha": "dde61dd4f4873823da7b7071f12958433cc87ad1a3b503d025506d77204360c8",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel10-arm64-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel10_x86_64": {
        "platform_name": "rhel10",
        "sha": "983bf4656b180713496fcc48b5a91bfc82db3e037eb399ebc5759e94117c1f81",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel10-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "ae717babeeebc484b1e597e3f73bbe3a36bab46acb541ea093dcae6924ae2ef6",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel82-arm64-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "e3b8aa095c6531997156f09fcb7e2502657a6f511b3b42817b4808864408dce5",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel81-ppc64le-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "4412c5b79436959361894445ad2c90da740907fd3acfefbd650392a856ce969e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel80-zseries-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "f28843500d206337af7172de633b63b21edb3ad8536e87d6261c02d56c514e12",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel80-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "142fd6b25bf47dabc71f6086f7dbf37c49c36265c75209fdf4638b90455610b6",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel90-arm64-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel9_ppc64le": {
        "platform_name": "rhel90-ppc64le",
        "sha": "1fd7f0cf60f950e96439c712b76e642e655d334b23d9768f50fe26d16e8adf4d",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel90-ppc64le-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel9_s390x": {
        "platform_name": "rhel90-zseries",
        "sha": "31663c778abd241eb1ae68d40ab9bc93ac1921a0701fd8f26c990466c9d0b59f",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel90-zseries-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "e735bb498db85bf6bf77982c0cd1cfb68a8cb452cbb73d7e3adab80460a48f5a",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel90-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "6a5cddab314d2998075b89f835840bca56a98f41f34a1dbd029481f739451f59",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-suse15-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "ubuntu18_x86_64": {
        "platform_name": "ubuntu1804",
        "sha": "197b95ed692bdeed58d7268a4047189a668a60927477e6e8616fbde1d2a18869",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu1804-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "5a3ba7601bd7e199a4e489e58ecfea9ee6c5ea7b7faf270bf1b1f62fce193113",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2004-arm64-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "4403bd4b9e7a850a1dbdb7a929711d485251326842a0e03f95d07d9f83def59d",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2004-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "6540024f475776ad78c8c02cb45001ad07a5de6c400d7f558e0b8a9b60b14ca2",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2204-arm64-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "c3a870943e5a5dca0bd87fc6b9e97c96a2b43c47531dc2e90f792683eacd82f5",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2204-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "12fc1e0cd8630b40d28d4779540bee1779b08106bde295cdf86a541023bd0f2e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2404-arm64-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "783f810e1bfac14e0afb31145af56df7457870145cf60c68546c4dcf35e8fc11",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2404-7553752a78d885cc2f60c725bfb5cd66e4c4e02b.tar.gz",
    },
}
