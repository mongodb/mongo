# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_ID = "a579bff08e941a4dae705e4708ec8da38805b6f1"
TOOLCHAIN_MAP_V5 = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "6b354f600ba17b5c3c3bfc2d042080b1b1385af22ad4322de8262a11c6e276a7",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2023-arm64-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "ed8ee504ae77fe59e30bcd2014330b29216bd07bfbee9a4fa9bed2888626b6da",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2023-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "4d5bda7bc06716e02ab92e55834c51b58a2ee92a40bebd064f898504c36d7aa0",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2-arm64-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "dcd6db2b2c498ea160707c6c9aa16bd7a136e62057ce41b77d3c15651d3559e5",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "debian10_x86_64": {
        "platform_name": "debian10",
        "sha": "112a7efb72b6193f3ef238e82736e670c412e5fded292ce1904fa6e0c5dd73df",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-debian10-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "1b1b100aca7f614e84366e1f95a3ee6ecb118542618c951f4d4eb4a07dc84e94",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-debian12-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "1c95f26668aba00f8b4c47b88a384fa9cf7ae456746ca1d99ce872841ed5454c",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel82-arm64-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "f1a391ffc8c55f98ac3b745df522c83b5d8a26b275c16eb05547ebbf63ed480c",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel81-ppc64le-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "7e8c20e7f6dc32d7d920679d80068dfc95ed710ec6af62e377b921bf3ab308ed",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel80-zseries-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "bd4920312168e2e1be19347e673201d0c48bb82feb030f0f5297ee5ad9266c0e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel80-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "195c1ded3597d6ee9f7c72d3222c259fc99d6e22281d1627d3e43ab5452cae6c",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-arm64-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "rhel9_ppc64le": {
        "platform_name": "rhel90-ppc64le",
        "sha": "a6e38753a1b46d8f6e5a1ac69e74c0a47e05eb97a0b132ef876aa3296794a25f",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-ppc64le-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "rhel9_s390x": {
        "platform_name": "rhel90-zseries",
        "sha": "c52c5df9f3b06ac9637c13cf9fff7d401a660f3c6d4fd181c7f6c0da7ca51879",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-zseries-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "bedecdddd9a83d5b307bbb5d8aa48b3ec298234483c1a5edf407594b429ef7c0",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "rhel10_x86_64": {
        "platform_name": "rhel10",
        "sha": "4f8bf9a3ce2454a6ce2636e45746dc2a25de139413fe2e60c28aac9b44b07df9",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel10-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "rhel10_aarch64": {
        "platform_name": "rhel10",
        "sha": "52869b742dc8a3550d9656187a4c0e8f95fe8bea542949c25717557cbfbafd42",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel10-arm64-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "14bcc17c41082207b9a033bd879ad66f2195a8581bb4fb0d8c03159d14ae30e1",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-suse15-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "ubuntu18_x86_64": {
        "platform_name": "ubuntu1804",
        "sha": "efe28c18d043969b5a67d0169eb97302d5cda62b8840dba2381f2fd74ab30f64",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu1804-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "37e712c413d94fe964b1c4de39def8b8fa0da572afdf4d72b38958590b79f848",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2004-arm64-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "f8a1a9c7a69bd35442cd494d44526283a49f592b4e517f2f8dcee96c8767504e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2004-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "52463a520f71acef28df10bbe57ce4395543b37868a16c32062ea7f38652064e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2204-arm64-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "79a3a981ccd9791890a7fe784a685d0f036c99aaf63f2578ee7d57685c2f13ac",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2204-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "645d3490927ea8ad25dde6ceccf88d7783b09f5a68935cceb2d85a12d0431508",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2404-arm64-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "8aede2636d06b85d275985a6ccaf7196d51a9edcd8553f10f166afa0f874b514",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2404-a579bff08e941a4dae705e4708ec8da38805b6f1.tar.gz",
    },
}
