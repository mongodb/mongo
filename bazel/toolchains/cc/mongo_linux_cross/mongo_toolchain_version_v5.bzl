"""Published GCC cross toolchains from toolchain-builder PR 153."""

S390X_GCC_TOOLCHAINS = {
    "rhel8": {
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel82-arm64-fd8e37ded721d4d8584efd3150136c04dd793121.tar.gz",
        "sha": "818b0e54e7ce6295c2127b6e22f4ad88bd2e4c87bd8ed4e654d27d4d5fa95f77",
    },
    "rhel9": {
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel90-arm64-fd8e37ded721d4d8584efd3150136c04dd793121.tar.gz",
        "sha": "c867d639311ec1e759bbad6e133ec16bd1b68e1dca0a58cab4075445ba6b1e79",
    },
    "rhel10": {
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel10-arm64-fd8e37ded721d4d8584efd3150136c04dd793121.tar.gz",
        "sha": "043d15c8143b53cc6ab55d7d4ec2b4150442c65bb80e62d9aeecdca200e0d473",
    },
}

# Published PPC GCC 14.2 compilers and runtimes from toolchain-builder PRs 154 and 156.
PPC64LE_GCC_TOOLCHAINS = {
    "rhel8": {
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel82-arm64-1bce2cbf33a4c08af2e73c3b2084e282ca76f3f5.tar.gz",
        "sha": "c65310e94cd23965b24315b7a5f87aff8444e9bc7189a414699ee19a530e4d79",
    },
    "rhel9": {
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel90-arm64-1bce2cbf33a4c08af2e73c3b2084e282ca76f3f5.tar.gz",
        "sha": "cd1d45638f2e0c27c2a0317744c158119110d88d3bc3ec78a11b72c092b0864d",
    },
    "rhel10": {
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_toolchain-rhel10-arm64-1bce2cbf33a4c08af2e73c3b2084e282ca76f3f5.tar.gz",
        "sha": "99ebd8a3e997ed8872b6bceb29a8bf4c5efd345d93444972691dbfb59b5c66cd",
    },
}
