set -o errexit

cd src
# Move binaries to CWD as Jepsen expects that.
mv dist-test/bin/* .
