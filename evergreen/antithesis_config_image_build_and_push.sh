DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit

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
$python buildscripts/resmoke.py generate-docker-compose --suite ${suite}
cp mongo-debugsymbols.tgz antithesis/antithesis_config/${suite}/debug

cd antithesis/antithesis_config/${suite}
sed -i s/evergreen-latest-master/$tag/ docker-compose.yml
sudo docker build . -t ${suite}-config:$tag

# Push Image
# login, push, and logout
echo "${antithesis_repo_key}" > mongodb.key.json
cat mongodb.key.json | sudo docker login -u _json_key https://us-central1-docker.pkg.dev --password-stdin
rm mongodb.key.json

sudo docker tag "${suite}-config:$tag" "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/${suite}-config:$tag"
sudo docker push "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/${suite}-config:$tag"
