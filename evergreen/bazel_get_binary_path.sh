bazel_get_binary_path() {
  ARCH="$(uname -m)"

  if [[ "$ARCH" == "ppc64le" || "$ARCH" == "ppc64" || "$ARCH" == "ppc" || "$ARCH" == "ppcle" || "$ARCH" == "s390x" || "$ARCH" == "s390" ]]; then
    return "bazel/bazelisk.py"
  else
    return "bazel"
  fi
}
