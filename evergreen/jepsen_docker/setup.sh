set -euo pipefail

if [ ! $(which docker) ]; then
  sudo apt-get update
  sudo apt-get install -yq \
    apt-transport-https \
    ca-certificates \
    curl \
    gnupg \
    lsb-release

  if [ ! -f "/usr/share/keyrings/docker-archive-keyring.gpg" ]; then
    curl -fsSL https://download.docker.com/linux/debian/gpg | sudo gpg --dearmor -o /usr/share/keyrings/docker-archive-keyring.gpg
  fi

  set +e
  if ! grep "https://download.docker.com/linux/debian" "/etc/apt/sources.list.d/docker.list"; then
    echo \
      "deb [arch=amd64 signed-by=/usr/share/keyrings/docker-archive-keyring.gpg] https://download.docker.com/linux/debian \
            $(lsb_release -cs) stable" | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
  fi
  set -e

  sudo apt-get update
  sudo apt-get install -yq docker-ce docker-ce-cli containerd.io
  sudo docker run hello-world
fi

if [ ! $(which docker-compose) ]; then
  sudo curl -L "https://github.com/docker/compose/releases/download/1.29.2/docker-compose-$(uname -s)-$(uname -m)" -o /usr/bin/docker-compose
  sudo chmod +x /usr/bin/docker-compose
fi

sudo chmod 777 /var/run/docker.sock

git clone --branch=evergreen-master git@github.com:10gen/jepsen.git jepsen
cp -rf src/dist-test jepsen/docker/node
# place the mongodb jepsen test adjacent to the control node's Dockerfile.
# it'll get copied into the image during the build process
git clone --branch=no-download-master git@github.com:10gen/jepsen-io-mongodb.git jepsen/docker/control/mongodb

# kill any running containers
sudo docker container kill $(docker ps -q) || true
