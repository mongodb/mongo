is_ppc64le() {
    ARCH="$(uname -m)"

    if [[ "$ARCH" == "ppc64le" || "$ARCH" == "ppc64" || "$ARCH" == "ppc" ]]; then
        return 0
    else
        return 1
    fi
}

is_s390x() {
    ARCH="$(uname -m)"

    if [[ "$ARCH" == "s390x" || "$ARCH" == "s390" ]]; then
        return 0
    else
        return 1
    fi
}

is_s390x_or_ppc64le() {
    if is_ppc64le || is_s390x; then
        return 0
    else
        return 1
    fi
}

bazel_get_binary_path() {
    if is_s390x_or_ppc64le; then
        echo "bazel/bazelisk.py"
    elif grep -q "ID=debian" /etc/os-release; then
        echo "bazel/bazelisk.py"
    elif grep -q 'ID="sles"' /etc/os-release; then
        echo "bazel/bazelisk.py"
    else
        echo "bazel"
    fi
}
