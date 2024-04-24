DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit

# The antithesis docker repository to push images to
antithesis_repo="us-central1-docker.pkg.dev/molten-verve-216720/mongodb-repository"

# tag images as evergreen[-${antithesis_build_type}]-{latest,patch} or just ${antithesis_image_tag}
if [ -n "${antithesis_image_tag:-}" ]; then
  echo "Using provided tag: '$antithesis_image_tag' for docker pushes"
  tag=$antithesis_image_tag
else
  tag="evergreen"
  if [[ -n "${antithesis_build_type}" ]]; then
    tag="${tag}-${antithesis_build_type}"
  fi

  if [ "${is_patch}" = "true" ]; then
    tag="${tag}-patch"
  else
    tag="${tag}-latest-${branch_name}"
  fi
fi

# Clean up any leftover docker artifacts
sudo docker logout
sudo docker rm $(docker ps -a -q) --force || echo "No pre-existing containers"
sudo docker network prune --force

# Login
echo "${antithesis_repo_key}" > mongodb.key.json
cat mongodb.key.json | sudo docker login -u _json_key https://us-central1-docker.pkg.dev --password-stdin
rm mongodb.key.json

# Build Image
cd src
activate_venv
setup_db_contrib_tool
$python buildscripts/resmoke.py run --suite ${suite} ${resmoke_args} --dockerComposeTag $tag --dockerComposeBuildImages workload,config,mongo-binaries --dockerComposeBuildEnv evergreen

# Test Image
docker-compose -f docker_compose/${suite}/docker-compose.yml up -d
echo "ALL RUNNING CONTAINERS: "
docker ps
set +o errexit
docker exec workload buildscripts/resmoke.py run --suite ${suite} ${resmoke_args} --sanityCheck --externalSUT
RET=$?
set -o errexit

docker-compose -f docker_compose/${suite}/docker-compose.yml logs > docker_logs.txt
docker-compose -f docker_compose/${suite}/docker-compose.yml down

# Change the permissions of all of the files in the docker compose directory to the current user.
# Some of the data files cannot be archived otherwise.
sudo chown -R $USER docker_compose/${suite}/
if [ $RET -ne 0 ]; then
  echo "Resmoke sanity check has failed"
  exit $RET
fi

# Push Image
sudo docker tag "${suite}:$tag" "$antithesis_repo/${task_name}:$tag"
sudo docker push "$antithesis_repo/${task_name}:$tag"

sudo docker tag "mongo-binaries:$tag" "$antithesis_repo/mongo-binaries:$tag"
sudo docker push "$antithesis_repo/mongo-binaries:$tag"

sudo docker tag "workload:$tag" "$antithesis_repo/workload:$tag"
sudo docker push "$antithesis_repo/workload:$tag"

# Logout
sudo docker logout https://us-central1-docker.pkg.dev
