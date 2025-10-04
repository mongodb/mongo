# Use bazel/platforms/remote_execution_containers_generator.py to generate this mapping for a given patch build.

REMOTE_EXECUTION_CONTAINERS = {
    "amazon_linux_2": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:aee76e9590f718e6745d4f0695dd7ce0bb258002a7a90408b8942bc4f2ce1cde",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:aee76e9590f718e6745d4f0695dd7ce0bb258002a7a90408b8942bc4f2ce1cde",
    },
    "amazon_linux_2023": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:88544e218a458fa659ce6de8e9b01b1e8855389a77c306e534ba2cafb1c198bf",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2023/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:88544e218a458fa659ce6de8e9b01b1e8855389a77c306e534ba2cafb1c198bf",
    },
    "debian10": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:4917278b646f5cd298013cad8a6bf645a258d14a22b93b5389975d2db80baff4",
        "dockerfile": "bazel/remote_execution_container/debian10/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:4917278b646f5cd298013cad8a6bf645a258d14a22b93b5389975d2db80baff4",
    },
    "debian12": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:d1df0fd76f9fdf28813fbe25dc9b989b5228f94cd37eae11fb194f67d3afdbbb",
        "dockerfile": "bazel/remote_execution_container/debian12/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:d1df0fd76f9fdf28813fbe25dc9b989b5228f94cd37eae11fb194f67d3afdbbb",
    },
    "linux": {
        "_COMMENT": "Uses amazon linux 2 container",
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:66b4f75708d3f845918b91a7fafb53b9c06612f3bca8eb87a179a206655357ca",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:66b4f75708d3f845918b91a7fafb53b9c06612f3bca8eb87a179a206655357ca",
    },
    "rhel8": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:f78343a8ea45c6e25284ec34c0faa2b3fdce088762441c12fcba7c162b10d765",
        "dockerfile": "bazel/remote_execution_container/rhel89/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:f78343a8ea45c6e25284ec34c0faa2b3fdce088762441c12fcba7c162b10d765",
    },
    "rhel9": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:908f34e965161a34088e212f0a09250a1e601f0400472e1ae2beabdfa377b5e5",
        "dockerfile": "bazel/remote_execution_container/rhel93/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:908f34e965161a34088e212f0a09250a1e601f0400472e1ae2beabdfa377b5e5",
    },
    "suse15": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:5295903271760e7e555590fb8858c1f7a74d3c0dd109d8ca200130ce07343ea4",
        "dockerfile": "bazel/remote_execution_container/suse/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:5295903271760e7e555590fb8858c1f7a74d3c0dd109d8ca200130ce07343ea4",
    },
    "ubuntu18": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:fad0b715ba85885f23eb43292014529bd4b9946e1ca1b825c2d8c9443f95a3e1",
        "dockerfile": "bazel/remote_execution_container/ubuntu18/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:fad0b715ba85885f23eb43292014529bd4b9946e1ca1b825c2d8c9443f95a3e1",
    },
    "ubuntu20": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:ed2d5bd04f0f23dbb57ff4e0f6d73f732e87b5ec976f7283908003a45b98a82e",
        "dockerfile": "bazel/remote_execution_container/ubuntu20/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:ed2d5bd04f0f23dbb57ff4e0f6d73f732e87b5ec976f7283908003a45b98a82e",
    },
    "ubuntu22": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:c1e4291aa0a1054e5c119a6c6a2ac3b43b2903e2f2c68d30148da7b57691ee9d",
        "dockerfile": "bazel/remote_execution_container/ubuntu22/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:c1e4291aa0a1054e5c119a6c6a2ac3b43b2903e2f2c68d30148da7b57691ee9d",
    },
    "ubuntu24": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:f1bd96104b3b8d33fff1500421917f3e15d9525795043e8d3f7f382e87c10002",
        "dockerfile": "bazel/remote_execution_container/ubuntu24/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:f1bd96104b3b8d33fff1500421917f3e15d9525795043e8d3f7f382e87c10002",
    },
}
