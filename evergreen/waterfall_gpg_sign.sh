set -o errexit
set -o pipefail

cd src

echo "GRS_CONFIG_USER1_USERNAME=${GARASIGN_USERNAME}" >>"signing-envfile"
echo "GRS_CONFIG_USER1_PASSWORD=${GARASIGN_PASSWORD}" >>"signing-envfile"

function gen_checksums() {
    shasum -a 1 $1 | tee $1.sha1
    shasum -a 256 $1 | tee $1.sha256
    md5sum $1 | tee $1.md5
}

cat <<'EOF' >gpg_signing_commands.sh
gpgloader
function sign(){
    gpg --yes -v --armor -o $1.sig --detach-sign $1
}
EOF

for filename in "$@"; do
    echo "generating checksums for '$filename'..."
    gen_checksums $filename

    cat <<EOF >>gpg_signing_commands.sh
        sign $filename
EOF
done

echo "executing batch signing command:"
cat ./gpg_signing_commands.sh

podman run \
    --env-file=signing-envfile \
    --rm \
    -v $(pwd):$(pwd) -w $(pwd) \
    ${GARASIGN_IMAGE} \
    /bin/bash -c "$(cat ./gpg_signing_commands.sh)"
