set -o errexit

cd src
git clone --branch=jepsen-mongodb-master --depth=1 git@github.com:10gen/jepsen.git jepsen-mongodb
cd jepsen-mongodb
branch=$(git symbolic-ref --short HEAD)
commit=$(git show -s --pretty=format:"%h - %an, %ar: %s")
echo "Git branch: $branch, commit: $commit"

lein install
