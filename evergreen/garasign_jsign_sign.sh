DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

if [ "${push_name}" != "windows" ]; then
    exit 0
fi

cd src

echo "GRS_CONFIG_USER1_USERNAME=${garasign_jsign_username}" >>"signing-envfile"
echo "GRS_CONFIG_USER1_PASSWORD=${garasign_jsign_password}" >>"signing-envfile"

set -o errexit
set -o verbose

msi_filename=mongodb-${push_name}-${push_arch}-${suffix}.msi
cp bazel-bin/src/mongo/installer/msi/mongodb-win32-x86_64-windows-${version}.msi $msi_filename

if [ "${is_patch}" != "true" ]; then
    # signing windows artifacts with jsign
    cat <<'EOF' >jsign_signing_commands.sh
function sign(){
  if [ -e $1 ]
  then
    jsign -a mongo-authenticode-2024 --replace --tsaurl http://timestamp.digicert.com -d SHA-256 $1
  else
    echo "$1 does not exist. Skipping signing"
  fi
}
EOF
    cat <<EOF >>jsign_signing_commands.sh
sign $msi_filename
EOF

    podman run \
        --env-file=signing-envfile \
        --rm \
        -v $(pwd):$(pwd) -w $(pwd) \
        ${garasign_jsign_image_ecr} \
        /bin/bash -c "$(cat ./jsign_signing_commands.sh)"
else
    echo "Not signing windows msi due to it being a patch build"
fi

# generating checksums
if [ -e $msi_filename ]; then
    shasum -a 1 $msi_filename | tee $msi_filename.sha1
    shasum -a 256 $msi_filename | tee $msi_filename.sha256
    md5sum $msi_filename | tee $msi_filename.md5
else
    echo "$msi_filename does not exist. Skipping checksum generation"
fi
