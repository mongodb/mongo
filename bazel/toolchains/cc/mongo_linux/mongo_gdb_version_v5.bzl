# Use mongo/bazel/toolchains/mongo_toolchain_version_generator.py to generate this mapping for a given patch build.

TOOLCHAIN_ID = "e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd"
TOOLCHAIN_MAP_V5 = {
    "amazon_linux_2023_aarch64": {
        "platform_name": "amazon2023-arm64",
        "sha": "35b5a1e65f0ef093a335003e004bf679215adcdf04988d1fcd0e2a2ebfba9a11",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2023-arm64-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "amazon_linux_2023_x86_64": {
        "platform_name": "amazon2023",
        "sha": "7f426456d7492a85ee27b3d091b322080842bf9bc554ee58126de959c2a179c0",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2023-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "amazon_linux_2_aarch64": {
        "platform_name": "amazon2-arm64",
        "sha": "d18508525aa688def6ea1f2c9dff41025e4f2805be06b4b06c122b0ce0225a62",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2-arm64-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "amazon_linux_2_x86_64": {
        "platform_name": "amazon2",
        "sha": "dd688bbdab117289ddc10d49bc845465a2cbab484b9506a2e94cd4f0999e0b73",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-amazon2-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "debian10_x86_64": {
        "platform_name": "debian10",
        "sha": "ad3148d8431e2b4d59c5e5ca5666da4ac595705f13892ed426531af4b721f745",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-debian10-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "debian12_x86_64": {
        "platform_name": "debian12",
        "sha": "8e90d3ee0d0f2fcb01823ad305c0da3a3325d2d444d72faa4b58c849b28898fe",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-debian12-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "rhel8_aarch64": {
        "platform_name": "rhel82-arm64",
        "sha": "e3b8f4029bdaba8cc651923f2bb1b00fa98c292661bb3b20b3844d99c83f9677",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel82-arm64-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "rhel8_ppc64le": {
        "platform_name": "rhel81-ppc64le",
        "sha": "c7b305b523dcdebb60f9e4816a346a68512b3d8582d28030c9c1e92fce9ac63f",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel81-ppc64le-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "rhel8_s390x": {
        "platform_name": "rhel80-zseries",
        "sha": "882791361c8c9342e6077b0bba0dd7dc836b79733d87df4fa782ebd82aafd459",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel80-zseries-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "rhel8_x86_64": {
        "platform_name": "rhel80",
        "sha": "89c93598ee74437d942d9912b7d1d4eee91509c463129d0fbde34edf38552e48",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel80-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "rhel9_aarch64": {
        "platform_name": "rhel90-arm64",
        "sha": "f2258a3790fd218d38a4aa46d6cdd2808599b575b6d709c4286f853959ae55b3",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-arm64-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "rhel9_ppc64le": {
        "platform_name": "rhel90-ppc64le",
        "sha": "ff3f0e3822c7dcd2671e10005b571752659f3ec891e35944ef8c698fc4282cea",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-ppc64le-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "rhel9_s390x": {
        "platform_name": "rhel90-zseries",
        "sha": "a42fec995cd17c784739eb58aab14dd3f360219791690b899918befac7a59d12",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-zseries-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "rhel9_x86_64": {
        "platform_name": "rhel90",
        "sha": "7fbbf982b84461daf9dc92cced727341c088d434726e99d00aa573a146c54bb1",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-rhel90-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
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
        "sha": "6cb677ab126f091a2940597fbd551dcc65497f045406989e5283b7b4af62d485",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-suse15-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "ubuntu18_x86_64": {
        "platform_name": "ubuntu1804",
        "sha": "8a41f2fe2229da16e110fbb6aaae6cb0166104996645823cf0bbcf1777a98705",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu1804-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "ubuntu20_aarch64": {
        "platform_name": "ubuntu2004-arm64",
        "sha": "9389650ff9b3970b34202e70ae9b1f5076b5bd24cc03cad949947237df1f7f60",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2004-arm64-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "ubuntu20_x86_64": {
        "platform_name": "ubuntu2004",
        "sha": "eee64048853a95385b969670cc72a61ecf0a1794798a6a4ff6acb54907ae7ac8",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2004-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "ubuntu22_aarch64": {
        "platform_name": "ubuntu2204-arm64",
        "sha": "45bd4ef3898dd4ef8999d578a282d0693272e7f0b66fb0d29539631b73e53f4d",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2204-arm64-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "ubuntu22_x86_64": {
        "platform_name": "ubuntu2204",
        "sha": "8a137776d93ec875af42ad87d718d08be0a5a5ffdec568557f1d3a7e311f29bd",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2204-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "ubuntu24_aarch64": {
        "platform_name": "ubuntu2404-arm64",
        "sha": "f5cdf5ca33b6f387952ca69550434d0a76fab309400b9d99380c47b4c497809a",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2404-arm64-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
    "ubuntu24_x86_64": {
        "platform_name": "ubuntu2404",
        "sha": "c57c869d58cc4a1397e8286bc7971b22f4f21d792b3c783af76a5eca749a75f9",
        "url": "https://s3.amazonaws.com/boxes.10gen.com/build/toolchain/bazel_v5_gdb-ubuntu2404-e921fc32d5c23d7cdb5cf406b05bf16eb5ab8dbd.tar.gz",
    },
}
