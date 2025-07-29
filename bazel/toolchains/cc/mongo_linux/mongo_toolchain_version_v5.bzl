# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_ID = "868b3c714c67f4f245623d2f772d0fac95d936a2"
TOOLCHAIN_MAP_V5 = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "4eb42e6cb53713d20b0564e2b838a8933aed192cf3550511937610f1d7af16d8",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-amazon2023-arm64-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "3727be0fcdf813f7ae58a99d1e10930bb38ee27aae7c16f2b3541b1b2ece2ca6",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-amazon2023-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "79210cd4968d9305f6d9dd6cfb1e1a4a64b0b45814a487f7872bb6b93c30e223",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-amazon2-arm64-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "f16910c055ea9c43a2cbebe0b8f79c636034d468531a399654dc8a28badc5170",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-amazon2-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "debian10_x86_64": {
        "platform_name": "debian10",
        "sha": "385792c7e5ab7c0fedd4c42b51426f1d8bf6dd74fb8abc3ce7102c25b2a6ee30",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-debian10-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "ed7c2a23385788a55df894d7e1a45319224c240629ef9f35c6ffbc75f37a347e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-debian12-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "a08eff5ba1cc62770b7cd7d23ec39f9c67dc36ee8880e54d77beef43d6aea105",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel82-arm64-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "19906f9f6c924bf33d10d951adeef7e5688ed61fdee1f0931a0eabb875abc2c4",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel81-ppc64le-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "d8e7b8ff17df494f845f42ab561d0a927d7d9fbdfff4a6b3df4cecd8d7b342c3",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel80-zseries-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "694ead26d8043572af943225ae96d80a88faac27a6aabd4a3e5597b175637091",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel80-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "1f9da8cd2b9b886ff873c0290d5f6a77e5c2bb41eeeceab0103f7b4674b622e5",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel90-arm64-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "rhel9_ppc64le": {
        "platform_name": "rhel90-ppc64le",
        "sha": "57d592e2d1c8d70b58f8f3e501fe8f692b9c1983a0d9d38e769afe27ccadad1b",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel90-ppc64le-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "rhel9_s390x": {
        "platform_name": "rhel90-zseries",
        "sha": "74a370c5e9b1027411c611985cf350437b27781a2caf7634b3cac4d4a2c306a1",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel90-zseries-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "37cced6fcc404a9921b5f7b209563a286029e8665e58efb7429f355c4f0ae8b9",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel90-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "0ef5d6d5e8a5823c1ccce8f216575cbd87ed6eb4d96afcdf268d6fdf91ff37bb",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-suse15-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "987cc35d9ca863f6c02e39f3e0149d08f156fd004ed047ec331d66ddbd57b0d8",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2004-arm64-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "20d1a0279644d26ab238bc3012ba93b5dbf0015e2e59bb4b4fe954bc912943a7",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2004-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "65571d2a359642a50bcd3bd2ad85fa811c0ae1ef0cb8a515fd0717841506ae2c",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2204-arm64-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "a935f81ba054dd453e97947a6fcdafe0c88df17bc9d2d23f177eac5f0749aa25",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2204-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "65eb508b6cd2831bfed18200b71b2053829cfc25628e61c1cbbf170564754b9f",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2404-arm64-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "58c0fddd2d31b7491bc4f03de779d8eedcfcd250fe469e85cbeb37dd191cdce3",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-ubuntu2404-868b3c714c67f4f245623d2f772d0fac95d936a2.tar.gz",
    },
}
