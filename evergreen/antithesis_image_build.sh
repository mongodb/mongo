DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -euo pipefail

# prepare the image building environment
cp -rf src/buildscripts/antithesis/ antithesis
# due to gitignore, we can't commit a folder called logs, so make it here
mkdir -p antithesis/logs/database{1,2,3}

# copy ... to the build context
# resmoke
cp -rf src antithesis/workload/src
# mongo binary
cp src/dist-test/bin/mongo antithesis/workload
# libvoidstar
cp /usr/lib/libvoidstar.so antithesis/workload/
# these aren't needed for the workload image, so get rid of them
rm -rf antithesis/workload/src/dist-test
# all mongodb binaries
cp -rf src/dist-test antithesis/database
cp /usr/lib/libvoidstar.so antithesis/database/

cd antithesis/workload
# push images as evergreen-latest-${branch_name}, unless it's a patch
tag="evergreen-latest-${branch_name}"
if [ "${is_patch}" = "true" ]; then
  tag="evergreen-patch"
fi
docker build . -t workload:$tag
cd ../database
docker build . -t database:$tag
cd ..
docker build . -t config:$tag

# login, push, and logout
echo "${antithesis_repo_key}" > mongodb.key.json
cat mongodb.key.json | docker login -u _json_key https://us-central1-docker.pkg.dev --password-stdin
rm mongodb.key.json

# tag and push to the registry
docker tag workload:$tag us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/workload:$tag
docker push us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/workload:$tag

docker tag database:$tag us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/database:$tag
docker push us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/database:$tag

docker tag config:$tag us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/config:$tag
docker push us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/config:$tag

docker logout https://us-central1-docker.pkg.dev
