DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -euo pipefail

# check that the binaries in dist-test are linked to libvoidstar
ldd src/dist-test/bin/mongod | grep libvoidstar
ldd src/dist-test/bin/mongos | grep libvoidstar
ldd src/dist-test/bin/mongo | grep libvoidstar

# prepare the image building environment
cp -rf src/buildscripts/antithesis/ antithesis

# copy ... to the build context
# resmoke
cp -rf src antithesis/base_images/workload/src
# mongo binary
cp src/dist-test/bin/mongo antithesis/base_images/workload
# libvoidstar
cp /usr/lib/libvoidstar.so antithesis/base_images/workload
# these aren't needed for the workload image, so get rid of them
rm -rf antithesis/base_images/workload/src/dist-test
# all mongodb binaries
cp -rf src/dist-test antithesis/base_images/mongo_binaries
cp /usr/lib/libvoidstar.so antithesis/base_images/mongo_binaries/

# push images as evergreen-latest-${branch_name}, unless it's a patch
tag="evergreen-latest-${branch_name}"
if [ "${is_patch}" = "true" ]; then
  tag="evergreen-patch"
fi

if [ -n "${antithesis_image_tag:-}" ]; then
  echo "Using provided tag: '$antithesis_image_tag' for docker pushes"
  tag=$antithesis_image_tag
fi

# Build Image
cd antithesis/base_images/mongo_binaries
sudo docker build . -t mongo-binaries:$tag

cd ../workload
sudo docker build . -t workload:$tag

# Push Image
# login, push, and logout
echo "${antithesis_repo_key}" > mongodb.key.json
cat mongodb.key.json | sudo docker login -u _json_key https://us-central1-docker.pkg.dev --password-stdin
rm mongodb.key.json

# tag and push to the registry
sudo docker tag "mongo-binaries:$tag" "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/mongo-binaries:$tag"
sudo docker push "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/mongo-binaries:$tag"

sudo docker tag "workload:$tag" "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/workload:$tag"
sudo docker push "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/workload:$tag"

sudo docker logout https://us-central1-docker.pkg.dev
