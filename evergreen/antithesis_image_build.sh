DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -euo pipefail

cd src
commit_date=$(date -d "$(git log -1 -s --format=%ci)" "+%s")
last_run_date=$(cat ../antithesis_last_push.txt || echo 0)
if [ "${is_patch}" != "true" && $last_run_date -gt $commit_date ]; then
  echo -e "Refusing to push new antithesis images because this commit is older\nthan the last pushed commit"
  exit 0
fi
cd ..

# check that the binaries in dist-test are linked to libvoidstar
ldd src/dist-test/bin/mongod | grep libvoidstar
ldd src/dist-test/bin/mongos | grep libvoidstar
ldd src/dist-test/bin/mongo | grep libvoidstar

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

if [ -n "${antithesis_image_tag}" ]; then
  echo "Using provided tag: '$antithesis_image_tag' for docker pushes"
  tag=$antithesis_image_tag
fi

docker build . -t workload:$tag
cd ../database
docker build . -t database:$tag
cd ..
# ensure that the embedded image references actually point to the images we're
# pushing here
sed -i s/evergreen-latest-master/$tag/ docker-compose.yml
docker build . -t config:$tag

# login, push, and logout
echo "${antithesis_repo_key}" > mongodb.key.json
cat mongodb.key.json | docker login -u _json_key https://us-central1-docker.pkg.dev --password-stdin
rm mongodb.key.json

# tag and push to the registry
docker tag "workload:$tag" "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/workload:$tag"
docker push "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/workload:$tag"

docker tag "database:$tag" "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/database:$tag"
docker push "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/database:$tag"

docker tag "config:$tag" "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/config:$tag"
docker push "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/config:$tag"

docker logout https://us-central1-docker.pkg.dev

if [ "${is_patch}" != "true" ]; then
  echo "$commit_date" > antithesis_next_push.txt
fi
