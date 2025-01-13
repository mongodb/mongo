set -euo pipefail

# Clone our internal fork of jepsen-io/jepsen to get the core
# functionality with a few tweaks meant for evergreen integration.
git clone --branch=v0.2.0-evergreen-master https://x-access-token:${jepsen_github_token}@github.com/10gen/jepsen.git jepsen

# Copy our mongodb source for jepsen to run into the docker area to be
# copied into the image during the build process.
cp -rf src/dist-test jepsen/docker/node

# Clone our internal tests to run
git clone --branch=v0.2.1 https://x-access-token:${jepsen_io_github_token}@github.com/10gen/jepsen-io-mongodb.git jepsen/docker/control/mongodb

# Kill any running containers
sudo docker container kill $(docker ps -q) || true
