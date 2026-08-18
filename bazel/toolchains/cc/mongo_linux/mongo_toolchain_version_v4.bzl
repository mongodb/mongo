# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_ID = "0c3eeaa558572479d43c6526bcb5cf9b16bdafe8"
TOOLCHAIN_MAP_V4 = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "a610eb3d81a3da44c297a570c1bdfd28f2807916e9d9dc55ae20ea20b04d3122",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-amazon2023-arm64-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "ba3a1bf18f14320621bd11df38231b72d8130b7fedc2007b74e8702450ad974a",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-amazon2023-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "77bdcdfc66a2b95fecf865a3de3a1b41d945e23244dc0f77b103322477b369dd",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-amazon2-arm64-93d85cc1a00ca1d53fcc7d4ca790f19a4e5dd542.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "8d7387f4829bf387c980b5d51e4176f2e0f9fd123081dff0c32982ea6d684cce",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-amazon2-93d85cc1a00ca1d53fcc7d4ca790f19a4e5dd542.tar.gz",
    },
    "debian10_x86_64": {
        "platform_name": "debian10",
        "sha": "0cbe29fb7fec714e1d160250d8a3a9ef00a46bdc097994e187f89eef5715489d",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-debian10-09adc0bbd90e6ad7d1daefbde3a1f4b146f0ec19.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "6e4b1434214646edabe9674808c3f6e674761c358b556283bee023f05a205ef7",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-debian12-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "rhel10_aarch64": {
        "platform_name": "rhel10-arm64",
        "sha": "e58ecf79959be353b79ccb3869fb8eb38a0fa68dd34b1c37fddaf9e5460c06dd",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel10-arm64-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "rhel10_x86_64": {
        "platform_name": "rhel10",
        "sha": "1609b402f0f5883a8a53b0c9faa392aff15fafbcef75852232beb8b1e425ba0d",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel10-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "b192f09bef4637709da8e3ff62c95195147d6f0240d8db05d035944335475812",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel82-arm64-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "561ea4af1cd085e53daad1b699c54951aba59e2501c767801a05f251a1a30b20",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel81-ppc64le-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "36b57ff284839a9e7f63e74c8b883a253db3916cea3af6855a97deff53cc13d1",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel80-zseries-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "9a1bc1558556dc1fc3002f609c5240f2fb2acff3c73f5ae25bb13dc5f706bacb",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel80-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "4c8ed3dd5943c19b99eca0a649eb496412a46f76b5f22e6505a1bd3758c8236f",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel90-arm64-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "rhel9_ppc64le": {
        "platform_name": "rhel90-ppc64le",
        "sha": "10d0e66ba030b7ae350fc8b58d51ce43a0fe1d5fc0b78912ff8aa6ec6a0a3ca5",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel90-ppc64le-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "rhel9_s390x": {
        "platform_name": "rhel90-zseries",
        "sha": "276155b472e25fd2d11a7e3edcdfcb138672addff69717902403014dc589a32b",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel90-zseries-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "5ce601befad753ece5e457813beff24189c7d10331577beb2d17b8aad551da87",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel90-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "54fecf1ea184ca224c99eca96d8a55b4d1446322ca2a90958356811c1ac71776",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-suse15-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "ubuntu18_x86_64": {
        "platform_name": "ubuntu1804",
        "sha": "638e008ef03c315dfb2c2f72953501484f300ea50a372d69e05341d348f9a3af",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-ubuntu1804-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "3e2906e592c48a70b1849ab7c2564c68c06c40e1dd8ba3b0786469490fdc1146",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-ubuntu2004-arm64-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "18a6cfe9337a716857f2c63543d0bbc0866a7384806df5e71f2b1ef8cf121a1e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-ubuntu2004-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "a9d8ac7dd0f4e7021de1af94d7e7ee61e2f7c2f156329fdf39271f0c4c61ba94",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-ubuntu2204-arm64-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "4bf631f044d951c9c78eb8633557e77b3acfdecbe6676291acf328b873ffca7a",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-ubuntu2204-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "32b657a4ef489bee4ff2731223158fceca2959d7853bb7f86dbaab71bfb6b1ab",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-ubuntu2404-arm64-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "970f07bf17fce627a417dde3044540e1d5fefa3be4f29d4bf60d13d2b6778043",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-ubuntu2404-0c3eeaa558572479d43c6526bcb5cf9b16bdafe8.tar.gz",
    },
}
