set -o errexit
set -o verbose

cd src

echo "GRS_CONFIG_USER1_USERNAME=${GARASIGN_USERNAME}" >> "signing-envfile"
echo "GRS_CONFIG_USER1_PASSWORD=${GARASIGN_PASSWORD}" >> "signing-envfile"

# this is a bit unfortunate, but we need to do this in order to generate checksum files with
# the proper filename in them, and we can't do a quick move in inline shell
# because shell.exec is disallowed.
local_filename=$1
publish_filename=$2

mv $local_filename $publish_filename

# msi_filename=mongodb-${push_name}-${push_arch}-${suffix}.msi
# cp bazel-bin/src/mongo/installer/msi/mongodb-win32-x86_64-windows-${version}.msi $msi_filename

cat << 'EOF' > jsign_signing_commands.sh
function sign(){
  jsign -a mongo-authenticode-2024 --replace --tsaurl http://timestamp.digicert.com -d SHA-256 $1
}
EOF

cat << EOF >> jsign_signing_commands.sh
sign $publish_filename
EOF

echo "executing signing command:"
cat ./jsign_signing_commands.sh

podman run \
  --env-file=signing-envfile \
  --rm \
  -v $(pwd):$(pwd) -w $(pwd) \
  ${GARASIGN_IMAGE} \
  /bin/bash -c "$(cat ./jsign_signing_commands.sh)"

function gen_checksums() {
  shasum -a 1 $1 | tee $1.sha1
  shasum -a 256 $1 | tee $1.sha256
  md5sum $1 | tee $1.md5
}

gen_checksums $publish_filename
