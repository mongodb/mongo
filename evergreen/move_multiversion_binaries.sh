set -o verbose

cd src
# powercycle expects the binaries to be in dist-test/bin
mkdir -p dist-test/bin
mv /data/multiversion/* dist-test/bin/
