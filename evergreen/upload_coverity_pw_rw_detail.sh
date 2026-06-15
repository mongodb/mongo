#!/usr/bin/env bash
# Non-fatal upload of the Coverity PW.*/RW.* artifact to S3.
# Exits 0 gracefully when AWS credentials are not available (expected on patch builds).
# On nightly runs, aws_key and aws_secret are set via add_expansions_to_env and the
# upload succeeds. The S3 URL is printed to the task log for reference.

set -uo pipefail

artifact="${workdir}/coverity_pw_rw_detail.gz"
s3_path="s3://mciuploads/${project}/${build_variant}/${revision}/coverity_pw_rw_detail.gz"

if [ ! -f "$artifact" ]; then
    echo "INFO: PW/RW artifact not found at $artifact — skipping upload"
    exit 0
fi

if [ -z "${aws_key:-}" ] || [ -z "${aws_secret:-}" ]; then
    echo "INFO: AWS credentials not configured — skipping upload (expected on patch builds)"
    echo "  Artifact is available locally at: $artifact"
    exit 0
fi

echo "Uploading $artifact → $s3_path"
if AWS_ACCESS_KEY_ID="${aws_key}" \
    AWS_SECRET_ACCESS_KEY="${aws_secret}" \
    aws s3 cp "$artifact" "$s3_path" \
    --storage-class STANDARD_IA \
    --content-type application/gzip; then
    echo "Upload complete: ${s3_path}"
else
    echo "WARN: Upload failed (non-fatal) — artifact is at $artifact"
fi
exit 0
