# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_ID = "868b3c714c67f4f245623d2f772d0fac95d936a2"
TOOLCHAIN_MAP_V5 = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "bc1d90826bfa78be59cf7783636184ffcf69aedd211712ddf02968f363d55814",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2023-arm64-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "be2e72b00e1f07ac3e8e76be6e5f635a778fe9809d9cc05f401d6a0e552e2944",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2-arm64-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "22456638329e30f72fdc66aaa7eb09ead65acf812c4ac1625a7cceaa744a645a",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "debian10_x86_64": {
        "platform_name": "debian10",
        "sha": "62e44b3698d340159acbd7cfbb32119a8ec47d9f66030426f1208682873fdecd",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-debian10-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "a11ad9a271ce7827eb9f2d73f79008e5eebf52ae24217e09abd8155f57b72a9d",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-debian12-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "cf5bf73a400dd4c3d219b9228329157530f21006b3851eb460f3f3906a5fcc36",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel82-arm64-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "16f9086d41d6716c236587abf21fd20dd30c93c990e6ae5d554b014f8c21c419",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel81-ppc64le-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "f95b5160733d36ac2e07d56b45da08148284b4687da10b7f631c86ff6ebeba1e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel80-zseries-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "c56a3e89977716f4ce016ef5d1694960ce0c26725c4f4ae6f8e249178e76965d",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel80-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "dc54a4eb4ac58572af2c5be5ddf956a522716134aa41aa67c0adf87d3fe4727e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-arm64-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "rhel9_ppc64le": {
        "platform_name": "rhel90-ppc64le",
        "sha": "d45760490972a5cc891e4d1b9b7452a0435c981ec0adf30e0a0e0497480077f2",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-ppc64le-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "rhel9_s390x": {
        "platform_name": "rhel90-zseries",
        "sha": "218ea3f2257f6eeb12b6f47f0953e709a7bf86241a607195d7ae30c122875ac0",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-zseries-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "ed2150c2658d676dc6ef9f4dceb5d224d5a2babaa85579c0d867137eeb4c18c1",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "60f69fac0dab9fc46e69d746a92627d0fbed6904ae202160fcd22ac296232d95",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-suse15-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "a3161982b7343fa584fc0c97749195d30d6b2c68ca1bd3696a540d07d89db282",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2004-arm64-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "d559fb106f967edce6ca322dcca3a573330f330ed7a39d481eb804d2b07f929a",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2004-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "f4c722385b64f3364b2203fa33e21018dc44ce6a111a76197b3b291635d60e07",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2204-arm64-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "172506128db28df34bd3d045f2eed18963a657b56eaf6ba6e9d1d49b50d308ba",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2204-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "1c845b8e1641c269e775be04b0bc7cb893a87d73f8e3cba9ef4e16903659edfb",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2404-arm64-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "99f602943b840fa0699d69498caf0e13e2c9ce0fb1fc0af2e6099a70ccb6596c",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2404-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
}
