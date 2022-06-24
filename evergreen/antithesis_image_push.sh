DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -euo pipefail

# push images as evergreen-latest-${branch_name}, unless it's a patch
tag="evergreen-latest-${branch_name}"
if [ "${is_patch}" = "true" ]; then
  tag="evergreen-patch"
fi

if [ -n "${antithesis_image_tag:-}" ]; then
  echo "Using provided tag: '$antithesis_image_tag' for docker pushes"
  tag=$antithesis_image_tag
fi

# login, push, and logout
echo "${antithesis_repo_key}" > mongodb.key.json
cat mongodb.key.json | sudo docker login -u _json_key https://us-central1-docker.pkg.dev --password-stdin
rm mongodb.key.json

# tag and push to the registry
sudo docker tag "mongo-binaries:$tag" "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/mongo-binaries:$tag"
sudo docker push "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/mongo-binaries:$tag"

sudo docker tag "workload:$tag" "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/workload:$tag"
sudo docker push "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/workload:$tag"

sudo docker tag "repl-set-config:$tag" "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/repl-set-config:$tag"
sudo docker push "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/repl-set-config:$tag"

sudo docker tag "sharded-cluster-config:$tag" "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/sharded-cluster-config:$tag"
sudo docker push "us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository/sharded-cluster-config:$tag"

sudo docker logout https://us-central1-docker.pkg.dev
