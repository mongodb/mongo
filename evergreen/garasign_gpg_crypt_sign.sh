DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

echo "GRS_CONFIG_USER1_USERNAME=${garasign_gpg_username_80}" >>"signing-envfile"
echo "GRS_CONFIG_USER1_PASSWORD=${garasign_gpg_password_80}" >>"signing-envfile"

set -o errexit
set -o verbose

ext="${ext:-tgz}"

crypt_file_name=mongo_crypt_shared_v1-${push_name}-${push_arch}-${suffix}.${ext}
mv "mongo_crypt_shared_v1.$ext" $crypt_file_name

# generating checksums
shasum -a 1 $crypt_file_name | tee $crypt_file_name.sha1
shasum -a 256 $crypt_file_name | tee $crypt_file_name.sha256
md5sum $crypt_file_name | tee $crypt_file_name.md5

# signing crypt linux artifact with gpg
cat <<EOF >>gpg_signing_commands.sh
gpgloader # loading gpg keys.
gpg --yes -v --armor -o $crypt_file_name.sig --detach-sign $crypt_file_name
EOF

podman run \
    --env-file=signing-envfile \
    --rm \
    -v $(pwd):$(pwd) -w $(pwd) \
    ${garasign_gpg_image_ecr} \
    /bin/bash -c "$(cat ./gpg_signing_commands.sh)"
