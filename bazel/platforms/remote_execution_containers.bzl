REMOTE_EXECUTION_CONTAINERS = {
    "linux_amd64": {
        # debian gcc based image contains the base our toolchain needs (glibc version and build-essentials)
        # https://hub.docker.com/layers/library/gcc/12.3-bookworm/images/sha256-6a3a5694d10299dbfb8747b98621abf4593bb54a5396999caa013cba0e17dd4f?context=explore
        "container-image": "docker://docker.io/library/gcc@sha256:6a3a5694d10299dbfb8747b98621abf4593bb54a5396999caa013cba0e17dd4f",
    },
    "linux_arm64": {
        # debian gcc based image contains the base our toolchain needs (glibc version and build-essentials)
        # https://hub.docker.com/layers/library/gcc/12.3-bookworm/images/sha256-6a3a5694d10299dbfb8747b98621abf4593bb54a5396999caa013cba0e17dd4f?context=explore
        "container-image": "docker://docker.io/library/gcc@sha256:6a3a5694d10299dbfb8747b98621abf4593bb54a5396999caa013cba0e17dd4f",
    },
}
