cd src

set -o errexit
set -o verbose

curl -L https://github.com/bazelbuild/bazelisk/releases/download/v1.17.0/bazelisk-linux-arm64 --output ./bazelisk && chmod +x ./bazelisk
