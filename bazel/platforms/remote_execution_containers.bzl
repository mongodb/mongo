# Use bazel/platforms/remote_execution_containers_generator.py to generate this mapping for a given patch build.

REMOTE_EXECUTION_CONTAINERS = {
    "amazon_linux_2": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:4a31b76e37382dccf6fb14643dd553ae82b6593219ae088709e71a9328a40211",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:4a31b76e37382dccf6fb14643dd553ae82b6593219ae088709e71a9328a40211",
    },
    "amazon_linux_2023": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:669ba932a3186dd50a6602b2f4ca03cd0264c7ff26bbc076334c25fe0c764172",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2023/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:669ba932a3186dd50a6602b2f4ca03cd0264c7ff26bbc076334c25fe0c764172",
    },
    "debian10": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:8cc4cb035f0087147c2f3d7ad36744f6c7d3c030261548c6fb74f49af541223b",
        "dockerfile": "bazel/remote_execution_container/debian10/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:8cc4cb035f0087147c2f3d7ad36744f6c7d3c030261548c6fb74f49af541223b",
    },
    "debian12": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:133589e5068f02332da131cf4311bb313148c167441ad0e53a739bed83ad713e",
        "dockerfile": "bazel/remote_execution_container/debian12/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:133589e5068f02332da131cf4311bb313148c167441ad0e53a739bed83ad713e",
    },
    "linux": {
        "_COMMENT": "Uses amazon linux 2 container",
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:5533058e50ba1b90275bcb7bcde4fbba6fa7347cdb247b6ba71c87f7869f06c8",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:5533058e50ba1b90275bcb7bcde4fbba6fa7347cdb247b6ba71c87f7869f06c8",
    },
    "rhel8": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:1de75c72764052dc9cd8f3b24187b88cd9d0f0742e2e73299aaee743e6b416fc",
        "dockerfile": "bazel/remote_execution_container/rhel89/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:1de75c72764052dc9cd8f3b24187b88cd9d0f0742e2e73299aaee743e6b416fc",
    },
    "rhel9": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:dbe5bb1a41da5d395b5d6ec27d4ea1107ad626b7c1f63a89db17df6b1be62066",
        "dockerfile": "bazel/remote_execution_container/rhel93/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:dbe5bb1a41da5d395b5d6ec27d4ea1107ad626b7c1f63a89db17df6b1be62066",
    },
    "suse15": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:53a595f2a6c0d5e23cf4cfca836b21e043ba18f00f9446b328f8bc717aae766d",
        "dockerfile": "bazel/remote_execution_container/suse/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:53a595f2a6c0d5e23cf4cfca836b21e043ba18f00f9446b328f8bc717aae766d",
    },
    "ubuntu18": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:149cf1f98bfe4881054cb07d74bbebf18b9da824b59f576140ee05e2f3cb9ea0",
        "dockerfile": "bazel/remote_execution_container/ubuntu18/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:149cf1f98bfe4881054cb07d74bbebf18b9da824b59f576140ee05e2f3cb9ea0",
    },
    "ubuntu20": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:8c6d39014d66fea9d5b20e11ade6522da7cec7e736eaa28c1df6e3cf5b82c840",
        "dockerfile": "bazel/remote_execution_container/ubuntu20/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:8c6d39014d66fea9d5b20e11ade6522da7cec7e736eaa28c1df6e3cf5b82c840",
    },
    "ubuntu22": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:0d188ca6f1e45c013caf34b25fda9c50cda13d757eb4c13d2f6a65be24a5a8f0",
        "dockerfile": "bazel/remote_execution_container/ubuntu22/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:0d188ca6f1e45c013caf34b25fda9c50cda13d757eb4c13d2f6a65be24a5a8f0",
    },
    "ubuntu24": {
        "container-url": "docker://quay.io/mongodb/bazel-remote-execution@sha256:a990012e30733060535bc66d1267dd54e8ca59915c5033b3c2f1bdc05f709e52",
        "dockerfile": "bazel/remote_execution_container/ubuntu24/dockerfile",
        "web-url": "https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:a990012e30733060535bc66d1267dd54e8ca59915c5033b3c2f1bdc05f709e52",
    },
}
