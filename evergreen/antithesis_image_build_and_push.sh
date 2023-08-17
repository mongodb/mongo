DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit

# The antithesis docker repository to push images to
antithesis_repo="us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository"

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
cd src
activate_venv
$python buildscripts/resmoke.py generate-docker-compose --in-evergreen --tag $tag ${suite}

# Test Image
cd antithesis/antithesis_config/${suite}
bash run_suite.sh

# Push Image
# login, push, and logout
echo "${antithesis_repo_key}" > mongodb.key.json
cat mongodb.key.json | sudo docker login -u _json_key https://us-central1-docker.pkg.dev --password-stdin
rm mongodb.key.json

sudo docker tag "${suite}:$tag" "$antithesis_repo/${suite}:$tag"
sudo docker push "$antithesis_repo/${suite}:$tag"

sudo docker tag "mongo-binaries:$tag" "$antithesis_repo/mongo-binaries:$tag"
sudo docker push "$antithesis_repo/mongo-binaries:$tag"

sudo docker tag "workload:$tag" "$antithesis_repo/workload:$tag"
sudo docker push "$antithesis_repo/workload:$tag"

sudo docker logout https://us-central1-docker.pkg.dev
