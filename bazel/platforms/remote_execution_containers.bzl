# Use bazel/platforms/remote_execution_containers_generator.py to generate this mapping for a given patch build.

REMOTE_EXECUTION_CONTAINERS = {
    "amazon_linux_2": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:9c6213c1f59fb2e18e3cf983422cfb89b54776ac654416e18136bf93e1685d78",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:9c6213c1f59fb2e18e3cf983422cfb89b54776ac654416e18136bf93e1685d78",
    },
    "amazon_linux_2023": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:6e860a09434327b93c47c11b64d369f7e6b04429d5610a9b063118bcad560e85",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2023/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:6e860a09434327b93c47c11b64d369f7e6b04429d5610a9b063118bcad560e85",
    },
    "debian10": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:18dbe847c591cf21822aae1ba596da27dcd7a4cb1b5ca1e02449caad11718e4b",
        "dockerfile": "bazel/remote_execution_container/debian10/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:18dbe847c591cf21822aae1ba596da27dcd7a4cb1b5ca1e02449caad11718e4b",
    },
    "debian12": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:b6171ae7dba5e5e87df627bcf621ca06642ba1f51de509dc894fa5c2b668215a",
        "dockerfile": "bazel/remote_execution_container/debian12/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:b6171ae7dba5e5e87df627bcf621ca06642ba1f51de509dc894fa5c2b668215a",
    },
    "linux": {
        "_COMMENT": "Uses amazon linux 2 container",
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:309525376d94d86406ea6e4f6e6ecb0bf9f5b678402516ca0ce286eed27887b9",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:309525376d94d86406ea6e4f6e6ecb0bf9f5b678402516ca0ce286eed27887b9",
    },
    "rhel7": {
        "_COMMENT": "Uses rhel89 container",
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:63d038364801f57030b875ffed6ab0b1a4e495d520fa08620048c6230d80eaf9",
        "dockerfile": "bazel/remote_execution_container/rhel89/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:63d038364801f57030b875ffed6ab0b1a4e495d520fa08620048c6230d80eaf9",
    },
    "rhel8": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:d712b0093205bbcb96d6c3dcea615b9fc4af7fd32ae68cf4d0ffbfc1d36533c2",
        "dockerfile": "bazel/remote_execution_container/rhel89/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:d712b0093205bbcb96d6c3dcea615b9fc4af7fd32ae68cf4d0ffbfc1d36533c2",
    },
    "rhel9": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:d718a69fcc22e52a66f340c77fc3857e822cf388420a11130993b189dea8b63f",
        "dockerfile": "bazel/remote_execution_container/rhel93/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:d718a69fcc22e52a66f340c77fc3857e822cf388420a11130993b189dea8b63f",
    },
    "suse15": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:dd84ce6e6465417f9d7996bb04c6cd82f5654359004b1f840f8815c5e6164769",
        "dockerfile": "bazel/remote_execution_container/suse/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:dd84ce6e6465417f9d7996bb04c6cd82f5654359004b1f840f8815c5e6164769",
    },
    "ubuntu18": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:81def09a0b2f0a3af605e2d30a33d6fb89ac0b2df27f1d4e373aeea91697d05a",
        "dockerfile": "bazel/remote_execution_container/ubuntu18/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:81def09a0b2f0a3af605e2d30a33d6fb89ac0b2df27f1d4e373aeea91697d05a",
    },
    "ubuntu20": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:7d4f25930ac7e15c240c81b99d38fa4c9227e0ee790fc3c7b9950cf0b8e23cd9",
        "dockerfile": "bazel/remote_execution_container/ubuntu20/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:7d4f25930ac7e15c240c81b99d38fa4c9227e0ee790fc3c7b9950cf0b8e23cd9",
    },
    "ubuntu22": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:6c130acbd9016cf56a3c8bac8b6836663da93542dd1eb8fc6a44e33997774cdc",
        "dockerfile": "bazel/remote_execution_container/ubuntu22/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:6c130acbd9016cf56a3c8bac8b6836663da93542dd1eb8fc6a44e33997774cdc",
    },
}
