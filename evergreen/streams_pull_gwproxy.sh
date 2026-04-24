#!/usr/bin/env bash
# Pull the latest gwproxy image from the SRE ECR repository.
# Filters for OCI image index manifests, pulls the most recent by digest,
# and tags it locally as gwproxy-sre:latest.
#
# Writes gwproxy_image_expansion.yml with the local tag for Evergreen expansion.
set -o pipefail

REGISTRY="664315256653.dkr.ecr.us-east-1.amazonaws.com"
REPO="sre/gwproxy"

RAW_JSON=$(aws ecr describe-images \
    --registry-id 664315256653 \
    --repository-name "$REPO" \
    --region us-east-1 \
    --output json)

DIGEST=$(echo "$RAW_JSON" | python3 -c "
import json, sys
data = json.load(sys.stdin)['imageDetails']
images = [i for i in data if i.get('imageManifestMediaType') == 'application/vnd.oci.image.index.v1+json']
latest = sorted(images, key=lambda x: x['imagePushedAt'])[-1]
print(latest['imageDigest'])
")

echo "Pulling gwproxy image: $REGISTRY/$REPO@$DIGEST"
docker pull "$REGISTRY/$REPO@$DIGEST"
docker tag "$REGISTRY/$REPO@$DIGEST" gwproxy-sre:latest
echo "gwproxy_image: gwproxy-sre:latest" >gwproxy_image_expansion.yml
