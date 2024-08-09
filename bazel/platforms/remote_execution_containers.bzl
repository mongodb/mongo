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
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:f147b396a33ea443b6d15e1ad41ebddadb8dd2f4fcec7376036f8aba649f1676",
        "dockerfile": "bazel/remote_execution_container/debian10/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:f147b396a33ea443b6d15e1ad41ebddadb8dd2f4fcec7376036f8aba649f1676",
    },
    "debian12": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:27f95376c57306129c36a689f45d19a6932e4b91f86ac3e317e8202fbfc55e1c",
        "dockerfile": "bazel/remote_execution_container/debian12/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:27f95376c57306129c36a689f45d19a6932e4b91f86ac3e317e8202fbfc55e1c",
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
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:eb517902338241f66666619a885786ca92b87501d95ae22cf372b8d60c01bd4d",
        "dockerfile": "bazel/remote_execution_container/ubuntu18/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:eb517902338241f66666619a885786ca92b87501d95ae22cf372b8d60c01bd4d",
    },
    "ubuntu20": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:0c7f442b0013fbb93f49867164d1d37bc7d77ba0c832e88c8731904e0cc5ec43",
        "dockerfile": "bazel/remote_execution_container/ubuntu20/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:0c7f442b0013fbb93f49867164d1d37bc7d77ba0c832e88c8731904e0cc5ec43",
    },
    "ubuntu22": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:47b9cc1b0e178e4d3a9c2f4d995d4623ba9bc59a1f207127a0e8b15d0c26a99b",
        "dockerfile": "bazel/remote_execution_container/ubuntu22/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:47b9cc1b0e178e4d3a9c2f4d995d4623ba9bc59a1f207127a0e8b15d0c26a99b",
    },
    "ubuntu24": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:83c63a552747517a6405f91c38e634b7391d9458f1b81e5f92846ed589527295",
        "dockerfile": "bazel/remote_execution_container/ubuntu24/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:83c63a552747517a6405f91c38e634b7391d9458f1b81e5f92846ed589527295",
    },
}
