is_s390x_or_ppc64le() {
  ARCH="$(uname -m)"

  if [[ "$ARCH" == "ppc64le" || "$ARCH" == "ppc64" || "$ARCH" == "ppc" || "$ARCH" == "ppcle" || "$ARCH" == "s390x" || "$ARCH" == "s390" ]]; then
    return 0
  else
    return 1
  fi
}

bazel_get_binary_path() {
  ARCH="$(uname -m)"

  if is_s390x_or_ppc64le; then
    echo "bazel/bazelisk.py"
  else
    echo "bazel"
  fi
}
