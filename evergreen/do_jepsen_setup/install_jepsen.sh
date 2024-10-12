set -o errexit

cd src
git clone --branch=v0.2.0-jepsen-mongodb-master --depth=1 git@github.com:10gen/jepsen.git jepsen-mongodb
cd jepsen-mongodb

lein install
