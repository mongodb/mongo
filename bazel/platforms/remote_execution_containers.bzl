REMOTE_EXECUTION_CONTAINERS = {
    "linux_amd64": {
        # amazon linux 2 based image contains the base our toolchain needs (glibc version and build-essentials)
        # https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:529162c1dedd8f72a7b65ed4b9d98439290ba29c350c01da8083b72dfdcec6f0
        "container-image": "docker://quay.io/mongodb/bazel-remote-execution@sha256:529162c1dedd8f72a7b65ed4b9d98439290ba29c350c01da8083b72dfdcec6f0",
    },
    "linux_arm64": {
        # amazon linux 2 based image contains the base our toolchain needs (glibc version and build-essentials)
        # https://quay.io/repository/mongodb/bazel-remote-execution/manifest/sha256:4d376a21c811b537c039bd366341f4897fee5c3c03c8b7373ff4b4537a7a0b5f
        "container-image": "docker://quay.io/mongodb/bazel-remote-execution@sha256:4d376a21c811b537c039bd366341f4897fee5c3c03c8b7373ff4b4537a7a0b5f",
    },
}
