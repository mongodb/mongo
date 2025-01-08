set -o errexit

cd src

git clone --branch=v0.3.0-jepsen-mongodb-master --depth=1 https://x-access-token:${github_token}@github.com/10gen/jepsen.git jepsen-mongodb
cd jepsen-mongodb

lein install
