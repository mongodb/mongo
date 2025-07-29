# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_ID = "84bea98f485ef8c1af3f0612c56423fac4ea6256"
TOOLCHAIN_MAP_V4 = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "8da5d6be6439f3753da0ccb7cbbc312571947cae4c702cca4d64e58e3f659831",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-amazon2023-arm64-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "3513bf41e2b10b500ab346194dc5a803cc3cb4b66e7b74fdd8c6d76655e9116e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-amazon2023-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "38bee5fa50ed1c8c527edfe511a652205b2ac5519a5ff30aaed67608749a8d31",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-amazon2-arm64-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "61edcd47f2f6c177801617f1ce1be168bb7b70003f42431da09c0aa4ea05b527",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-amazon2-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "debian10_x86_64": {
        "platform_name": "debian10",
        "sha": "901199126d6a10d77537fdcc19332588e679da589ab71a6500c71bcd2a48167d",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-debian10-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "40e94b657eddbe3c8cf89678e51c1a6f34d4a8cd73335962aa909ebed62270e3",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-debian12-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "1c5ffab1382ee37c6f3b64ee9b908b56f7fafb5ee413068a312cbae6c9ddbb47",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel82-arm64-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "baa9c82fe02c41a2fcb142848bd55f584bd85030217ec151b363a499a4b7565e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel81-ppc64le-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "b45a31e88b8593290bfdaddfa6f81b518085096fa58f2dc6afab75ec4a23224f",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel80-zseries-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "168aaf1a31eb9ebaad6b2e82f234c9ec1018fe87215ea832e2a973eb3dab9026",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel80-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "85e61e65d3b8f4d469224ef6d8497a628173f76a8b6e2c534c81fb96a6728487",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel90-arm64-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "rhel9_ppc64le": {
        "platform_name": "rhel90-ppc64le",
        "sha": "9fb1f50358d2910dd8cc675c107b206ac8f06d7d490fb75aa4a0ee06dc113534",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel90-ppc64le-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "rhel9_s390x": {
        "platform_name": "rhel90-zseries",
        "sha": "9419449c6221a7d6ef084a6a0553e64d324b7adbddf6c8f664a40fb65cf6657e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel90-zseries-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "dbb449ece5981817e60f22fc72ac467991b17db6316bfeec655131d784181ef3",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel90-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "9464edde3999d4456cbcea0085c52b027547c3d8b52c17a408fe191ae26dabd5",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-suse15-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "ubuntu18_x86_64": {
        "platform_name": "ubuntu1804",
        "sha": "7c8f9e3e889ec0d32149ce872bedfce4e0c08a71741513a92d65993a7ecf131a",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-ubuntu1804-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "53caed99e13984bdcd9b666d41daece7e2c35d32dce1d994934ea5399cb6b6b0",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-ubuntu2004-arm64-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "9ac5e210b779ace2ae4fa9b4e9911f3c5cb8f20be81e87fa1ae129fcd93af798",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-ubuntu2004-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "eabe3ab69318800739d35de7bce772977ac447818d21043f77edea66a6b0730b",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-ubuntu2204-arm64-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "70769f8a36b32955b4fb4af5307ac50d4dc9813228befe2aa194e604d9f7cfba",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-ubuntu2204-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "ba617414633dce1ab5882337775ba0382a13ab9ea7138f82c7fb6519488822fc",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-ubuntu2404-arm64-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "fdd1930124357b255e60dbace9fcbe667d1042d573d50727633d26ed26ad973c",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-ubuntu2404-84bea98f485ef8c1af3f0612c56423fac4ea6256.tar.gz",
    },
}
