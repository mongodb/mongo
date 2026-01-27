# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_ID = "e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd"
TOOLCHAIN_MAP_V5 = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "9a108875e80d5ad9f3ff2c09c99da5951dedb66e9e3955397fad93783030db02",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-amazon2023-arm64-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "0b695d60913053f4d7194a02078e995be237051bd66ec622901e514b7ec6c10d",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-amazon2023-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "882fa1b2eff34c02058313106a6e2e674c8c88afaf1345d1a215dd984796d61f",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-amazon2-arm64-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "9428b646820413069bdda10d6cf14bce036866d0b9d3f031b2c1c82e2f5824d7",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-amazon2-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "debian10_x86_64": {
        "platform_name": "debian10",
        "sha": "f3441a16c478b833e4c822c1afd191328c8270ca22e23f81b62d5b3de5b9f21c",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-debian10-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "783294fe1418f4ad5ea522a3956f842517dd99297a2e694f7cc2abbc13acfd3c",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-debian12-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "2c49497e4c95577e67d8fd87923f76faf8140906b78c45a941dee7d90572ec4a",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel82-arm64-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "4d7f05fbc368794b5598e391e56e7e209cae71cc034e93d307b7f84d0c60e8b2",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel81-ppc64le-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "2e72e68190ea0210ee4e23c07bbcb699fd2e6f47d06ed6d69878c25eb2f28cbd",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel80-zseries-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "3dbf36b9db8c9fb8e9ad7f7697ce50b9360b4e23a26a47c92335cad4abef33dd",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel80-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "72beac176dc767365a34507792dc76ebfb1fff4733071aba97d341d52cfad9e6",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel90-arm64-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "rhel9_ppc64le": {
        "platform_name": "rhel90-ppc64le",
        "sha": "36d73a7dfa93fd9cc683ab4f6abb793ee53fabf6ff218cc9155ec3fbedb7ef32",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel90-ppc64le-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "rhel9_s390x": {
        "platform_name": "rhel90-zseries",
        "sha": "61d61b47b9fcd69180f423ba17cba61a68022fd2f7165a8fad25f2ab15cf7c29",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel90-zseries-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "68c06343ccaf9114ca4b817e182563ff1fbb7db987c1e7d822e530243544d52e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel90-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "rhel10_x86_64": {
        "platform_name": "rhel10",
        "sha": "5aa5980fa2fe96a2ddaf9c8c43a9850273e70af766e25150a0c700f91635eb11",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel10-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "rhel10_aarch64": {
        "platform_name": "rhel10",
        "sha": "664a86f9c6286b756009d901c46a5e3504a6d1c36ebb2ae813dfc0a0fdebaa49",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel10-arm64-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "2040c679283bf0572d38c28db5108b97b552fb06b15ba6788d494c066929e6d3",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-suse15-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "ubuntu18_x86_64": {
        "platform_name": "ubuntu1804",
        "sha": "1d4282320a0066295fab70cfff0008094462792c83ea0245e85e71ae3ef6a25c",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu1804-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "138334adc0c88d9297a25c1fa0523e161f3aa462431529a6669cc99892b308f3",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2004-arm64-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "d6625c21f710fdfd9a2ae71dc964a9f75b2e3b976bd6c8638d35ef4cda7d77dc",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2004-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "ff7ac45e2633e8b0e2574f369301126e6520985ff9e78290d804d8e47b7f8308",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2204-arm64-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "0dfccd57cc453e4708e6937b36e4f0e70de51379007b434d2cb88b1f09afe910",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2204-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "fb1e08443a0b71c5722fd8cd85a780f853947141fe47c131f3c50143f5bbaac8",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2404-arm64-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "44581b6a7e0df623cecc6a9f56c958f4a981293a9df338c5a95a5aa08310fd0a",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2404-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
}
