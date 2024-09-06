set -o errexit

cd src
# "v0.2.0-jepsen-mongodb-master" is the latest jepsen version that works with v5.0,
# because JS engine on v5.0 does not know the "import" statement, that was added in later jepsen versions
git clone --branch=v0.2.0-jepsen-mongodb-master --depth=1 git@github.com:10gen/jepsen.git jepsen-mongodb
cd jepsen-mongodb

lein install
