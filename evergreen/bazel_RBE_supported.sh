bazel_rbe_supported() {

  OS="$(uname)"
  ARCH="$(uname -m)"

  # TODO SERVER-85806 enable RE for amd64
  if [ "$OS" == "Linux" ] && [ "$ARCH" == "aarch64" ]; then
    return 0
  else
    return 1
  fi
}
