# Use bazel/platforms/remote_execution_containers_generator.py to generate this mapping for a given patch build.

REMOTE_EXECUTION_CONTAINERS = {
    "amazon_linux_2": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:6822e92c4d01aaa4deab68c0dda7d55704fe04e6d742b9cc0f28bc48042eedfc",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:6822e92c4d01aaa4deab68c0dda7d55704fe04e6d742b9cc0f28bc48042eedfc",
    },
    "amazon_linux_2023": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:e6080195e7c253f8013317c51ad5ae0fa3e593c77a8cf30390d7c4f4cf8395f3",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2023/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:e6080195e7c253f8013317c51ad5ae0fa3e593c77a8cf30390d7c4f4cf8395f3",
    },
    "debian10": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:4917278b646f5cd298013cad8a6bf645a258d14a22b93b5389975d2db80baff4",
        "dockerfile": "bazel/remote_execution_container/debian10/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:4917278b646f5cd298013cad8a6bf645a258d14a22b93b5389975d2db80baff4",
    },
    "debian12": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:24d21b038c38fa4bea9cef8913f62756b7df1faa8a893306f42fcb934b957504",
        "dockerfile": "bazel/remote_execution_container/debian12/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:24d21b038c38fa4bea9cef8913f62756b7df1faa8a893306f42fcb934b957504",
    },
    "linux": {
        "_COMMENT": "Uses amazon linux 2 container",
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:672e55d036e2eb3e795a483c7704ca5fffe2fbf0a0989b7374474dc380bdb9f0",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:672e55d036e2eb3e795a483c7704ca5fffe2fbf0a0989b7374474dc380bdb9f0",
    },
    "rhel10": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:b027821747bc93162db8757c86ef1747c7c464dd3c2a2392bb3005565d793811",
        "dockerfile": "bazel/remote_execution_container/rhel10/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:b027821747bc93162db8757c86ef1747c7c464dd3c2a2392bb3005565d793811",
    },
    "rhel8": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:a628a8b69f286232dd94b3646055b31a4afb29876af075c1cbf078f0b36cf882",
        "dockerfile": "bazel/remote_execution_container/rhel89/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:a628a8b69f286232dd94b3646055b31a4afb29876af075c1cbf078f0b36cf882",
    },
    "rhel9": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:94db357316d2d3ea32751a197378c7dbbf825f5afd470e46ba27fdd107a5427f",
        "dockerfile": "bazel/remote_execution_container/rhel93/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:94db357316d2d3ea32751a197378c7dbbf825f5afd470e46ba27fdd107a5427f",
    },
    "suse15": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:f93cde94f9db31ecf08385a086b64aa8b1327cf81ddd3ef8b879c552e0d90799",
        "dockerfile": "bazel/remote_execution_container/suse/Dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:f93cde94f9db31ecf08385a086b64aa8b1327cf81ddd3ef8b879c552e0d90799",
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
