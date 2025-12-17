DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
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

# Temporary fix until DEVPROD-6961 is complete
# move docker data dir out of the root partition
sudo service docker stop
sudo mkdir -p /data/mci/docker
if ! sudo jq -e . /etc/docker/daemon.json; then
    echo "docker daemon.json did not exist or was invalid"
    echo "setting docker daemon.json to {}"
    sudo sh -c 'echo "{}" > /etc/docker/daemon.json'
fi
MODIFIED_JSON=$(sudo jq '."data-root" |= "/data/mci/docker"' /etc/docker/daemon.json)
sudo echo "${MODIFIED_JSON}" | sudo tee /etc/docker/daemon.json
echo "docker daemon.json: set data-root to /data/mci/docker"
sudo service docker start

# Login
echo "${antithesis_repo_key}" >mongodb.key.json
cat mongodb.key.json | sudo docker login -u _json_key https://us-central1-docker.pkg.dev --password-stdin
rm mongodb.key.json

extra_args=""

if [ -n "${antithesis_test_composer_dir:-}" ]; then
    extra_args="--dockerComposeTestComposerDirs ${antithesis_test_composer_dir}"
fi

# Build Image
cd src
activate_venv
setup_db_contrib_tool
$python buildscripts/resmoke.py run --suite ${suite} ${resmoke_args} --dockerComposeTag $tag --dockerComposeBuildImages workload,config,mongo-binaries --dockerComposeBuildEnv evergreen ${extra_args}

# Test Image
docker-compose -f docker_compose/${suite}/docker-compose.yml up -d
echo "ALL RUNNING CONTAINERS: "
docker ps
set +o errexit
# add a 1 hr timeout so we have time to gather the logs and run the rest of the script
# when it hangs
timeout -v 3600 docker exec workload buildscripts/resmoke.py run --suite ${suite} ${resmoke_args} --sanityCheck --externalSUT
RET=$?
set -o errexit

docker-compose -f docker_compose/${suite}/docker-compose.yml logs >docker_logs.txt
docker-compose -f docker_compose/${suite}/docker-compose.yml down

# Change the permissions of all of the files in the docker compose directory to the current user.
# Some of the data files cannot be archived otherwise.
sudo chown -R $USER docker_compose/${suite}/
if [ $RET -ne 0 ]; then
    echo "Resmoke sanity check has failed"
    exit $RET
fi

# Push Config Image
sudo docker tag "${suite}:$tag" "$antithesis_repo/${task_name}:$tag"
sudo docker push "$antithesis_repo/${task_name}:$tag"

# Push workload and binary images with s3 lock to prevent multiple pushes across different tasks
set +o errexit
$python buildscripts/s3_lock.py --bucket mciuploads --key ${project}/${version_id}/${build_variant}/antithesis_lock
RET=$?
set -o errexit

if [ $RET -eq 0 ]; then
    echo "Aquired lock for workload and binary images, pushing to antithesis."
    sudo docker tag "mongo-binaries:$tag" "$antithesis_repo/mongo-binaries:$tag"
    sudo docker push "$antithesis_repo/mongo-binaries:$tag"

    sudo docker tag "workload:$tag" "$antithesis_repo/workload:$tag"
    sudo docker push "$antithesis_repo/workload:$tag"
elif [ $RET -eq 1 ]; then
    echo "Failed to acquire lock for workload and binary images, skipping push."
else
    echo "Error occurred when attempting to acquire s3 lock, exiting."
    exit 1
fi

# Logout
sudo docker logout https://us-central1-docker.pkg.dev

# Skip triggering tests for patch builds
if [ "${is_patch}" != "true" ]; then
    exit 0
fi

# Parameter was not passed to schedule antithesis tests
if [ "${schedule_antithesis_tests}" != "true" ]; then
    exit 0
fi

params=$(jq -n \
    --arg config_image "${task_name}:$tag" \
    --arg images "mongo-binaries:$tag;workload:$tag" \
    --arg description "Patch Test: ${task_name} for ${tag}" \
    --arg author_email "${author_email}" \
    '{
    params: {
      "antithesis.description": $description,
      "custom.duration": "1",
      "antithesis.config_image": $config_image,
      "antithesis.images": $images,
      "antithesis.report.recipients": $author_email,
      "antithesis.is_ephemeral": "true"
    }
  }')

echo "Calling Antithesis API endpoint: https://mongo.antithesis.com/api/v1/launch/${antithesis_endpoint}"
echo "With parameters:"
echo "$params" | jq .

curl --fail -u "mongodb:${antithesis_api_password}" \
    -X POST "https://mongo.antithesis.com/api/v1/launch/${antithesis_endpoint}" \
    -H "Content-Type: application/json" \
    -d "$params"

echo -e "\nSuccessfully triggered Antithesis ${antithesis_endpoint} test"
