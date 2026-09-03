REMOTE_EXECUTION_CONTAINERS = {
    "amazon_linux_2": {
        "container-url": "docker://public.ecr.aws/w3i8j1a8/devprod-build@sha256:f492854bacfd2320361e2a2b4a49fe70b18c9e1b030b1948de6938e17a8af667",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2/Dockerfile",
        "web-url": "https://gallery.ecr.aws/w3i8j1a8/devprod-build/sha256:f492854bacfd2320361e2a2b4a49fe70b18c9e1b030b1948de6938e17a8af667",
    },
    "amazon_linux_2023": {
        "container-url": "docker://public.ecr.aws/w3i8j1a8/devprod-build@sha256:b0762df86c5972c666704bc0a359070df62b227736a95a18238c03cfc832ca6a",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2023/Dockerfile",
        "web-url": "https://gallery.ecr.aws/w3i8j1a8/devprod-build/sha256:b0762df86c5972c666704bc0a359070df62b227736a95a18238c03cfc832ca6a",
    },
    "amazon_linux_2023_3": {
        "container-url": "docker://public.ecr.aws/w3i8j1a8/devprod-build@sha256:e4bcfd0c7aca4298b6275e469aeb330c967c29961eaaf71196eee9a546634acd",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2023_3/dockerfile",
        "web-url": "https://gallery.ecr.aws/w3i8j1a8/devprod-build/sha256:e4bcfd0c7aca4298b6275e469aeb330c967c29961eaaf71196eee9a546634acd",
    },
    "debian12": {
        "container-url": "docker://public.ecr.aws/w3i8j1a8/devprod-build@sha256:0ee680e10986ced1d82f6e6409b9c56c9583519820bf6ecb84099d526019477c",
        "dockerfile": "bazel/remote_execution_container/debian12/Dockerfile",
        "web-url": "https://gallery.ecr.aws/w3i8j1a8/devprod-build/sha256:0ee680e10986ced1d82f6e6409b9c56c9583519820bf6ecb84099d526019477c",
    },
    "linux": {
        "_COMMENT": "Uses amazon linux 2 container",
        "container-url": "docker://public.ecr.aws/w3i8j1a8/devprod-build@sha256:0ac4075d3a77a8228b966ee6bafb6bc4dcdcabc48a4eb240bb322c9c58526081",
        "dockerfile": "bazel/remote_execution_container/amazon_linux_2/Dockerfile",
        "web-url": "https://gallery.ecr.aws/w3i8j1a8/devprod-build/sha256:0ac4075d3a77a8228b966ee6bafb6bc4dcdcabc48a4eb240bb322c9c58526081",
    },
    "rhel10": {
        "container-url": "docker://public.ecr.aws/w3i8j1a8/devprod-build@sha256:baa0d5d66252ae9677f2d6d4592564fbe9a9c8eb9be785fa4994105386a8f4c8",
        "dockerfile": "bazel/remote_execution_container/rhel10/Dockerfile",
        "web-url": "https://gallery.ecr.aws/w3i8j1a8/devprod-build/sha256:baa0d5d66252ae9677f2d6d4592564fbe9a9c8eb9be785fa4994105386a8f4c8",
    },
    "rhel8": {
        "container-url": "docker://public.ecr.aws/w3i8j1a8/devprod-build@sha256:f6d34394547327b065901cc3b93aa67ffc8ae7fea6fcaf9a756ee5c9e3a5f4ca",
        "dockerfile": "bazel/remote_execution_container/rhel89/Dockerfile",
        "web-url": "https://gallery.ecr.aws/w3i8j1a8/devprod-build/sha256:f6d34394547327b065901cc3b93aa67ffc8ae7fea6fcaf9a756ee5c9e3a5f4ca",
    },
    "rhel9": {
        "container-url": "docker://public.ecr.aws/w3i8j1a8/devprod-build@sha256:d54ecd172102f33c18bb3e208d5f9d0de05a43283a2633ed36b8eb6a535f5cb3",
        "dockerfile": "bazel/remote_execution_container/rhel93/Dockerfile",
        "web-url": "https://gallery.ecr.aws/w3i8j1a8/devprod-build/sha256:d54ecd172102f33c18bb3e208d5f9d0de05a43283a2633ed36b8eb6a535f5cb3",
    },
    "suse15": {
        "container-url": "docker://public.ecr.aws/w3i8j1a8/devprod-build@sha256:2023f4c4f18ebfb538e8aafbe9343b3dc752b658a0f20a1114b5d408889c69e0",
        "dockerfile": "bazel/remote_execution_container/suse/Dockerfile",
        "web-url": "https://gallery.ecr.aws/w3i8j1a8/devprod-build/sha256:2023f4c4f18ebfb538e8aafbe9343b3dc752b658a0f20a1114b5d408889c69e0",
    },
    "ubuntu18": {
        "container-url": "docker://public.ecr.aws/w3i8j1a8/devprod-build@sha256:0a96b1c24020d0cc0ca9eb624105e38030f206c3b705de2002c27ff02c7cd0a2",
        "dockerfile": "bazel/remote_execution_container/ubuntu18/Dockerfile",
        "web-url": "https://gallery.ecr.aws/w3i8j1a8/devprod-build/sha256:0a96b1c24020d0cc0ca9eb624105e38030f206c3b705de2002c27ff02c7cd0a2",
    },
    "ubuntu20": {
        "container-url": "docker://public.ecr.aws/w3i8j1a8/devprod-build@sha256:e6f969b1c9613fe0d516fa4ba0a8f71c4621810eed9fad9785788cee2a98f93f",
        "dockerfile": "bazel/remote_execution_container/ubuntu20/Dockerfile",
        "web-url": "https://gallery.ecr.aws/w3i8j1a8/devprod-build/sha256:e6f969b1c9613fe0d516fa4ba0a8f71c4621810eed9fad9785788cee2a98f93f",
    },
    "ubuntu22": {
        "container-url": "docker://public.ecr.aws/w3i8j1a8/devprod-build@sha256:8d857b9e220b3bda5776c46a9fda1b740b77b72332dec941ba22abb785cf4f6c",
        "dockerfile": "bazel/remote_execution_container/ubuntu22/Dockerfile",
        "web-url": "https://gallery.ecr.aws/w3i8j1a8/devprod-build/sha256:8d857b9e220b3bda5776c46a9fda1b740b77b72332dec941ba22abb785cf4f6c",
    },
    "ubuntu24": {
        "container-url": "docker://public.ecr.aws/w3i8j1a8/devprod-build@sha256:c149ffd5df007a909a7bbb87719a96a4445c62bc2c0a9f1f609ae9ae6ae7e6b7",
        "dockerfile": "bazel/remote_execution_container/ubuntu24/Dockerfile",
        "web-url": "https://gallery.ecr.aws/w3i8j1a8/devprod-build/sha256:c149ffd5df007a909a7bbb87719a96a4445c62bc2c0a9f1f609ae9ae6ae7e6b7",
    },
}
