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
