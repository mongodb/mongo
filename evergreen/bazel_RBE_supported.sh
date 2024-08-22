bazel_rbe_supported() {

  OS="$(uname)"
  ARCH="$(uname -m)"

  if [ "$ARCH" == "aarch64" ] || [ "$ARCH" == "arm64" ] || [ "$ARCH" == "x86_64" ]; then
    return 0
  else
    return 1
  fi
}
