set -euo pipefail

git clone --branch=evergreen-master git@github.com:10gen/jepsen.git jepsen
cp -rf src/dist-test jepsen/docker/node
# place the mongodb jepsen test adjacent to the control node's Dockerfile.
# it'll get copied into the image during the build process
git clone --branch=no-download-master git@github.com:10gen/jepsen-io-mongodb.git jepsen/docker/control/mongodb

# kill any running containers
sudo docker container kill $(docker ps -q) || true
