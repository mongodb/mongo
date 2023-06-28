#!/bin/sh

set -o errexit
set -o verbose

tag="evergreen-latest"
if [ "${is_patch}" = "true" ]; then
    tag="evergreen-patch"
fi

if [ -n "${antithesis_image_tag:-}" ]; then
    echo "Using provided tag: '$antithesis_image_tag' for docker pushes"
    tag=$antithesis_image_tag
fi

# Generate the version file
VERSION_FILE=../../cmake_build/VERSION
echo "Branch: ${branch_name}" > $VERSION_FILE
echo "Build ID: ${build_id}" >> $VERSION_FILE
echo "Revision: ${revision}" >> $VERSION_FILE

# Build the containers
sudo docker build -f test_format.dockerfile -t wt-test-format:$tag ../..
sed -i s/wt-latest/$tag/ docker-compose.yaml
sudo docker build -f config.docker -t wt-test-format-config:$tag ../..

# login, push, and logout (expecting env var antithesis_repo_key to be available)
echo "${antithesis_repo_key}" > mongodb.key.json
cat mongodb.key.json | sudo docker login -u _json_key https://us-central1-docker.pkg.dev --password-stdin
rm mongodb.key.json

# tag and push to the registry
sudo docker tag "wt-test-format:$tag" "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/wt-test-format:$tag"
sudo docker push "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/wt-test-format:$tag"

sudo docker tag "wt-test-format-config:$tag" "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/wt-test-format-config:$tag"
sudo docker push "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/wt-test-format-config:$tag"

sudo docker logout https://us-central1-docker.pkg.dev
