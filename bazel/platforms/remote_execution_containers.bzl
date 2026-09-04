# Use bazel/platforms/remote_execution_containers_generator.py to generate this mapping for a given patch build.

REMOTE_EXECUTION_CONTAINERS = {
    "amazon_linux_2": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:6822e92c4d01aaa4deab68c0dda7d55704fe04e6d742b9cc0f28bc48042eedfc",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:6822e92c4d01aaa4deab68c0dda7d55704fe04e6d742b9cc0f28bc48042eedfc",
    },
    "amazon_linux_2023": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:d3a58bface87a8e0f54d6d5b8cda32285537863a8b2a2faa99821296a0ba3b15",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2023/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:d3a58bface87a8e0f54d6d5b8cda32285537863a8b2a2faa99821296a0ba3b15",
    },
    "amazon_linux_2023_3": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:da7b6cb457a7313f844ca196335f2af47e4fed81878974c5140dc97b48f3f399",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2023_3/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:da7b6cb457a7313f844ca196335f2af47e4fed81878974c5140dc97b48f3f399",
    },
    "debian10": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:4917278b646f5cd298013cad8a6bf645a258d14a22b93b5389975d2db80baff4",
        "dockerfile": "bazel/remote_execution_container/debian10/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:4917278b646f5cd298013cad8a6bf645a258d14a22b93b5389975d2db80baff4",
    },
    "debian12": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:d059e234a5e386fd276b8ee183d77be475082df39da9e832877dafaaf19d44ca",
        "dockerfile": "bazel/remote_execution_container/debian12/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:d059e234a5e386fd276b8ee183d77be475082df39da9e832877dafaaf19d44ca",
    },
    "linux": {
        "_COMMENT": "Uses amazon linux 2 container",
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:672e55d036e2eb3e795a483c7704ca5fffe2fbf0a0989b7374474dc380bdb9f0",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:672e55d036e2eb3e795a483c7704ca5fffe2fbf0a0989b7374474dc380bdb9f0",
    },
    "rhel10": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:791660d34432fdaa8faed729762ab4cf309cd60aaefc1ea7aaca05a8c469d443",
        "dockerfile": "bazel/remote_execution_container/rhel10/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:791660d34432fdaa8faed729762ab4cf309cd60aaefc1ea7aaca05a8c469d443",
    },
    "rhel8": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:dda9318ccf70f5f2061e8f1261bbb8fa43e31d39b741329a5a60494b06ca2caf",
        "dockerfile": "bazel/remote_execution_container/rhel89/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:dda9318ccf70f5f2061e8f1261bbb8fa43e31d39b741329a5a60494b06ca2caf",
    },
    "rhel9": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:fab4bb597071f10a3717cac3d84fa67232ea2998c2c1942f15a9bd68649a820f",
        "dockerfile": "bazel/remote_execution_container/rhel93/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:fab4bb597071f10a3717cac3d84fa67232ea2998c2c1942f15a9bd68649a820f",
    },
    "suse15": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:25bcb8a6ae6b872a90a2e4773ca0d5612f8f78d9c0eccc49539e7595037b01ca",
        "dockerfile": "bazel/remote_execution_container/suse/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:25bcb8a6ae6b872a90a2e4773ca0d5612f8f78d9c0eccc49539e7595037b01ca",
    },
    "ubuntu18": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:e515d710bea8964ffe6fd625899ee1ed8906a47a2b9915ade34c1e1d79329b46",
        "dockerfile": "bazel/remote_execution_container/ubuntu18/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:e515d710bea8964ffe6fd625899ee1ed8906a47a2b9915ade34c1e1d79329b46",
    },
    "ubuntu20": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:60ceea610a82ae03976d2d26f86212904cd6184f61cb31410712bdd0aef301db",
        "dockerfile": "bazel/remote_execution_container/ubuntu20/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:60ceea610a82ae03976d2d26f86212904cd6184f61cb31410712bdd0aef301db",
    },
    "ubuntu22": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:e9ef6a7caea4cf5c4858ebe54fc743163aaa58b1a6e55e970851abf85841424e",
        "dockerfile": "bazel/remote_execution_container/ubuntu22/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:e9ef6a7caea4cf5c4858ebe54fc743163aaa58b1a6e55e970851abf85841424e",
    },
    "ubuntu24": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:11c4f6d4848e0b1f44ab1d16f1c1b08555cbbad658579367e53b22b9540b987a",
        "dockerfile": "bazel/remote_execution_container/ubuntu24/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:11c4f6d4848e0b1f44ab1d16f1c1b08555cbbad658579367e53b22b9540b987a",
    },
}
