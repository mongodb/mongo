function is_macos() {
    local -r os="$(uname -s | tr '[:upper:]' '[:lower:]')"
    [[ "${os}" == "darwin" ]] && return 0 || return 1
}

function is_ppc64le() {
    local -r arch="$(uname -m)"
    [[ "${arch}" == "ppc64le" || "${arch}" == "ppc64" || "${arch}" == "ppc" ]] && return 0 || return 1
}

function is_s390x() {
    local -r arch="$(uname -m)"
    [[ "${arch}" == "s390x" || "${arch}" == "s390" ]] && return 0 || return 1
}

function is_s390x_or_ppc64le() {
    (is_ppc64le || is_s390x) && return 0 || return 1
}

function bazel_get_binary_path() {
    if is_macos; then
        echo "bazel"
    elif is_s390x_or_ppc64le ||
        grep -q "ID=debian" /etc/os-release ||
        grep -q 'ID="sles"' /etc/os-release; then
        echo "bazel/bazelisk.py"
    else
        echo "bazel"
    fi
}
