#!/usr/bin/env bash
set -euo pipefail

# Check if we're running under Bazel
if [[ -n "${BUILD_WORKSPACE_DIRECTORY-}" ]]; then
    # We're running under Bazel, use the workspace directory
    REPO_DIR="$BUILD_WORKSPACE_DIRECTORY"
else
    # We're not running under Bazel, use the Git root
    REPO_DIR=$(git rev-parse --show-toplevel 2>/dev/null)
    if [[ -z "$REPO_DIR" ]]; then
        echo "Error: Not in a Git repository and not running under Bazel."
        exit 1
    fi
fi

# Use the repository directory as the base for output. This should always be the
# same, regardless of invocation location.
cd "$REPO_DIR/bazel/remote_execution_container"

echo "Working directory: $(pwd)"

# Common packages for different distribution families. Packages you add here
# will end up in all images of this distribution family.
declare -A COMMON_PACKAGES
COMMON_PACKAGES["redhat"]="
cyrus-sasl-devel
cyrus-sasl-gssapi
glibc-devel
krb5-devel
libcurl-devel
openldap-devel
openssl-devel
systemtap-sdt-devel
"

COMMON_PACKAGES["debian"]="
build-essential
libcurl4-openssl-dev
libgssapi-krb5-2
libldap2-dev
libsasl2-dev
libssl-dev
libxml2-dev
libkrb5-dev
"

COMMON_PACKAGES["suse"]="
cyrus-sasl-devel
cyrus-sasl-gssapi
glibc-devel
krb5-devel
libcurl-devel
libopenssl-devel
openldap2-devel
"

# Distribution-specific additional packages. If you have a package that
# shouldn't be installed in all containers, use this section to add custom
# packages.
declare -A ADDITIONAL_PACKAGES
ADDITIONAL_PACKAGES["amazonlinux:2"]="
libzstd
"

ADDITIONAL_PACKAGES["amazonlinux:2023"]="
libzstd
"

ADDITIONAL_PACKAGES["redhat/ubi8:8.9"]="
ncurses-compat-libs
"

ADDITIONAL_PACKAGES["debian:10"]="
libfl2
"

ADDITIONAL_PACKAGES["opensuse/leap:15.2"]="
libfl2
"

ADDITIONAL_PACKAGES["ubuntu:18.04"]="
systemtap-sdt-dev
"

ADDITIONAL_PACKAGES["ubuntu:20.04"]="
systemtap-sdt-dev
"

ADDITIONAL_PACKAGES["ubuntu:22.04"]="
systemtap-sdt-dev
"

ADDITIONAL_PACKAGES["ubuntu:24.04"]="
systemtap-sdt-dev
libncurses-dev
"

# This maps container images to the output locations for the generated
# Dockerfiles.
declare -A IMAGE_DIRS
IMAGE_DIRS["amazonlinux:2"]="amazon_linux_2"
IMAGE_DIRS["amazonlinux:2023"]="amazon_linux_2023"
IMAGE_DIRS["debian:10"]="debian10"
IMAGE_DIRS["debian:12"]="debian12"
IMAGE_DIRS["redhat/ubi8:8.9"]="rhel89"
IMAGE_DIRS["redhat/ubi9:9.3"]="rhel93"
IMAGE_DIRS["opensuse/leap:15.2"]="suse"
IMAGE_DIRS["ubuntu:18.04"]="ubuntu18"
IMAGE_DIRS["ubuntu:20.04"]="ubuntu20"
IMAGE_DIRS["ubuntu:22.04"]="ubuntu22"
IMAGE_DIRS["ubuntu:24.04"]="ubuntu24"

# Fetch the latest sha256:xxx hash for an image.
get_latest_sha() {
    docker pull "$1" >/dev/null
    docker inspect --format='{{index .RepoDigests 0}}' "$1" | cut -d@ -f2
}

# Determine the package manager for the distribution.
get_package_manager() {
    local image="$1"
    if [[ "$image" == *"amazonlinux"* || "$image" == *"redhat"* ]]; then
        echo "yum"
    elif [[ "$image" == *"debian"* || "$image" == *"ubuntu"* ]]; then
        echo "apt"
    elif [[ "$image" == *"opensuse"* ]]; then
        echo "zypper"
    else
        echo "unknown"
    fi
}

# Expand the global distribution packages.
get_distribution_family() {
    local image="$1"
    if [[ "$image" == *"amazonlinux"* || "$image" == *"redhat"* ]]; then
        echo "redhat"
    elif [[ "$image" == *"debian"* || "$image" == *"ubuntu"* ]]; then
        echo "debian"
    elif [[ "$image" == *"opensuse"* ]]; then
        echo "suse"
    else
        echo "unknown"
    fi
}

# Find the pinned package versions.
get_package_versions() {
    local image="$1"
    shift
    local packages=("$@")
    local pkg_manager
    pkg_manager=$(get_package_manager "$image")

    case "$pkg_manager" in
    yum)
        docker run --rm "$image" bash -c "
                yum info ${packages[*]} 2>/dev/null | 
                awk '/^Name/ {name=\$3} /^Version/ {version=\$3} /^Release/ {release=\$3} 
                /^Release/ {print name \"-\" version \"-\" release}' | 
                sort -u"
        ;;
    apt)
        docker run --rm "$image" bash -c "
                apt-get update >/dev/null 2>&1 && 
                apt-cache policy ${packages[*]} | 
                awk '/^[^ ]/ {pkg=\$1} /Candidate:/ {print pkg \"=\" \$2}' | 
                sort -u"
        ;;
    zypper)
        # TODO(SERVER-93423): Pin suse package versions. At the moment this
        # breaks the remote_execution_containers_generator.py script.
        printf '%s\n' "${packages[@]}" | sort -u
        # docker run --rm "$image" bash -c "
        #     zypper --non-interactive refresh >/dev/null 2>&1 &&
        #     zypper --non-interactive info ${packages[*]} |
        #     awk '/^Name/ {name=\$3} /^Version/ {version=\$3} /^Version/ {print name \"=\" version}' |
        #     sort -u"
        ;;
    *)
        echo "Unsupported package manager for image: $image" >&2
        return 1
        ;;
    esac
}

# Write the Dockerfile.
generate_dockerfile() {
    local image="$1"
    local output_dir="${IMAGE_DIRS[$image]}"

    local distribution_family
    distribution_family=$(get_distribution_family "$image")

    readarray -t packages < <(echo "${COMMON_PACKAGES[$distribution_family]}" | sed '/^\s*$/d')
    readarray -t additional < <(echo "${ADDITIONAL_PACKAGES[$image]:-}" | sed '/^\s*$/d')
    packages+=("${additional[@]}")

    local sha
    sha=$(get_latest_sha "$image")
    local image_with_sha="${image}@${sha}"

    local pkg_manager
    pkg_manager=$(get_package_manager "$image")

    local install_lines
    install_lines=$(get_package_versions "$image" "${packages[@]}" | sed 's/^/        /' | sed 's/$/\ \\/')

    case "$pkg_manager" in
    yum)
        update_cmd="yum check-update || true"
        install_cmd="yum install -y"
        clean_cmd="&& yum clean all && rm -rf /var/cache/yum/*"
        ;;
    apt)
        update_cmd="apt-get update"
        install_cmd="DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends"
        clean_cmd="&& rm -rf /var/lib/apt/lists/*"
        ;;
    zypper)
        update_cmd="zypper refresh"
        install_cmd="zypper install -y --no-recommends"
        clean_cmd="&& zypper clean --all"
        ;;
    *)
        echo "Unsupported package manager for image: $image" >&2
        return 1
        ;;
    esac

    # Remove colons from package versions for Debian and Ubuntu
    if [[ "$pkg_manager" == "apt" ]]; then
        install_lines=${install_lines//:=/=}
    fi

    mkdir -p "$output_dir"
    cat <<EOF >"$output_dir/dockerfile"
# DO NOT EDIT.
#
# This Dockerfile is generated by the 'repin_dockerfiles.sh' script. To repin
# versions or change packages, edit that script instead.
#
# To repin the hashes:
#
#   bazel run \\
#       //bazel/remote_execution_container:repin_dockerfiles \\
#       --config=local
#
# To update the docker images, follow the instructions in the
# confluence page: go/devprod-build-update-rbe-containers.

FROM $image_with_sha

RUN $update_cmd && \\
    $install_cmd \\
$install_lines
    $clean_cmd

CMD ["/bin/bash"]
EOF

    echo "Generated Dockerfile for $image in $output_dir"
}

# Generating the Dockerfiles doesn't really put the CPU under load. Generate
# all of them asynchronously to speed up processing.

echo "Generating Dockerfiles..."
for image in "${!IMAGE_DIRS[@]}"; do
    (
        generate_dockerfile "$image"
    ) &
done

wait
echo "Done."
