# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_ID = "2a5f4757b341f678def9fda281807dd2243d9bc2"
TOOLCHAIN_MAP_V5 = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "28c501c4d7c65373b25c88a1b928d2c68e6e7f7dd90e073e0e5f529a772fc7f2",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2023-arm64-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "amazon_linux_2023_3_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "28c501c4d7c65373b25c88a1b928d2c68e6e7f7dd90e073e0e5f529a772fc7f2",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2023-arm64-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "591a6a66acb4e9c9dfd3ef80a139810442f67f409b3c7b3d6f48a7e3de4511c5",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2023-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "amazon_linux_2023_3_x86_64": {
        "platform_name": "amazon2023",
        "sha": "591a6a66acb4e9c9dfd3ef80a139810442f67f409b3c7b3d6f48a7e3de4511c5",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2023-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "5b2dcfdbc5a7bf2e612e507b7d1ff6a6179fe83b548ffa1c94913faedb01029c",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2-arm64-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "29ade9273b54b8dbb7620f60c61a588297471d63f722aa5862b6bc4fe8ac1a04",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "84b0f8fa1499fb6bd6c4324246265a7de4d8cb967b10fe30b05b5793cc6c96f8",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-debian12-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "debian13_x86_64": {
        "platform_name": "debian13",
        "sha": "a0121ada0a703cd6fa50204f33f3be65b8f5272fe5a32547ca945a9407174631",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-debian13-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "rhel10_aarch64": {
        "platform_name": "rhel10-arm64",
        "sha": "eb5a5cfa4f64e8d22c84519af85d4c63e4393834d4edefad6be827a76ce4c529",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel10-arm64-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "rhel10_ppc64le": {
        "platform_name": "rhel10-ppc64le",
        "sha": "23e7d066beaa82196cee271a11e7ea6ba21fdd6d9eb7ca43acd5a4f74da459f1",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel10-ppc64le-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "rhel10_s390x": {
        "platform_name": "rhel10-zseries",
        "sha": "6b2bdeb94f8ed1e46feef35b4037ee8b58b3ce15a5c411f12cd9fd95bcefabb0",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel10-zseries-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "rhel10_x86_64": {
        "platform_name": "rhel10",
        "sha": "72b76006384b20ad4c931966d5ebc048304568dd958835c27486579766edf9c5",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel10-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "3f98dd8590b1b2e32f1a27827b8b284533653bd6361267dad2cda2a19a22ebc5",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel82-arm64-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "eb1c821c600a654c15fd7a86905ac930415aed3d8696563f851161c997fffb99",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel81-ppc64le-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "968bb330cf9d9cca43b30b875ad365b80bd31774c7aaed75d3e2ac214c71beb2",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel80-zseries-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "501f136e4cea2da77ce783457aad88f7ffb781733d1fbd8e9f4fbd401acc1adb",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel80-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "1ccd907383a86ce792d6e6dadaffc08606de696e34a412b2528800dd0808883e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-arm64-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "rhel9_ppc64le": {
        "platform_name": "rhel90-ppc64le",
        "sha": "c30f716d4a361d12b1b3849b5cb2bfe4b30999f3b29722ba65dceed40664ddb3",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-ppc64le-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "rhel9_s390x": {
        "platform_name": "rhel90-zseries",
        "sha": "ff11336df1d3d35b54c2be75ed3718a0f5484329a5749df95e83c4239e177a32",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-zseries-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "7e91ae2811d05ebd24304eb5433af8dde6acbac97596e7696c103cdc26360646",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "suse15_x86_64": {
        "platform_name": "suse15",
        "sha": "9cc8f82524994e508e45537b7d7d3ff5bd829024f29bd299d0a6a795e516a66c",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-suse15-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "suse16_x86_64": {
        "platform_name": "suse16",
        "sha": "f76b24d30eac597b0f7ca0de78d8834ce44319122458ec338e748aeb77fa8aff",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-suse16-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "5a53c4c597ecdd002d339176828cbcd88ee86c64ed856476364198bc13dc8d74",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2004-arm64-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "e3f072f01c86b9cc338ed3af099e815017eecbd27cedf390d85e11c682e1bac9",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2004-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "645de78b54d6cc1f8a38700e491a4e2cec80f8064c571818a0e9e74560761715",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2204-arm64-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "804abd8f0823ee27457dc3366f8e5f085e9c69a77cecab9f9bee524fa1b3eb8e",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2204-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "b73aa07a5bebff6216d1d66f4b3385694cf8cbc67569244048b943710695db86",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2404-arm64-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "9d8460caabd24cdf677a9462244c6e499bf8a3244df2df429ea08c9d6fde4640",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2404-2a5f4757b341f678def9fda281807dd2243d9bc2.tar.gz",
    },
}
