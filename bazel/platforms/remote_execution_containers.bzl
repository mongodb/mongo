# Use bazel/platforms/remote_execution_containers_generator.py to generate this mapping for a given patch build.

REMOTE_EXECUTION_CONTAINERS = {
    "amazon_linux_2": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:57c9b945990dd2288c0dae78155d5478b5b5816a77ea5c632aeaf086b71b583b",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:57c9b945990dd2288c0dae78155d5478b5b5816a77ea5c632aeaf086b71b583b",
    },
    "amazon_linux_2023": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:06fc8103d2e2af9878ee7c5a792f32b3de1c8d68266c193f7ab35b7a136da519",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2023/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:06fc8103d2e2af9878ee7c5a792f32b3de1c8d68266c193f7ab35b7a136da519",
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
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:f80dfd1e9a9d1ea28225b6ecb8049e5789cc365176a0766422771d58dda3708f",
        "dockerfile": "bazel/remote_execution_container/rhel89/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:f80dfd1e9a9d1ea28225b6ecb8049e5789cc365176a0766422771d58dda3708f",
    },
    "rhel9": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:89dbd46e1e9540b8689bf5431a78afe0cc15906b1ba0d6babf00ee59aba574b3",
        "dockerfile": "bazel/remote_execution_container/rhel93/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:89dbd46e1e9540b8689bf5431a78afe0cc15906b1ba0d6babf00ee59aba574b3",
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
