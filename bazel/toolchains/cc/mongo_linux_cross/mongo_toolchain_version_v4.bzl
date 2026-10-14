"""Published GCC cross toolchains from toolchain-builder PRs 155/156."""

S390X_GCC_TOOLCHAINS = {
    "rhel8": {
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel82-arm64-1bce2cbf33a4c08af2e73c3b2084e282ca76f3f5.tar.gz",
        "sha": "e822329b902378abfe78540ec733bb665abf0e215c1d4be4e9e84b09240d97ee",
    },
    "rhel9": {
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel90-arm64-1bce2cbf33a4c08af2e73c3b2084e282ca76f3f5.tar.gz",
        "sha": "b695593c98bed9867ea3a449f50e8f617149467bf461ca82efd7957589cb925f",
    },
    "rhel10": {
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v4_toolchain-rhel10-arm64-1bce2cbf33a4c08af2e73c3b2084e282ca76f3f5.tar.gz",
        "sha": "1682c04fa0836c8b2156599ccf094f44639c3dafc0c40f28d1c4bfb49ced6d35",
    },
}
