#!/usr/bin/env bash
# Pull the mongot localdev image via the DevProd ECR Docker Hub pull-through cache.
# See: https://docs.devprod.prod.corp.mongodb.com/devprod-platforms-ecr/DockerHub
#
# Authentication (ec2.assume_role + docker login for 901841024863) must be done
# before this script runs — handled by the "assume devprod ECR role" and
# "login to devprod ECR" Evergreen functions in definitions.yml.
#
# Writes mongot_image_expansion.yml with the local tag for Evergreen expansion.
set -o errexit
set -o nounset
set -o pipefail

IMAGE="901841024863.dkr.ecr.us-east-1.amazonaws.com/dockerhub/mongodb/mongodb-atlas-search:latest"

echo "Pulling mongot localdev image: $IMAGE"
docker pull "$IMAGE"
docker tag "$IMAGE" mongot-localdev:latest
echo "mongot_image: mongot-localdev:latest" >mongot_image_expansion.yml
