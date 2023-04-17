DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose

cd src

long_ext=${ext}
if [ "$long_ext" == "tgz" ]; then
  long_ext="tar.gz"
fi

mv mongo-binaries.tgz mongodb-${push_name}-${push_arch}-${suffix}.${ext}
mv mongo-cryptd.tgz mongodb-cryptd-${push_name}-${push_arch}-${suffix}.${ext} || true
mv mh.tgz mh-${push_name}-${push_arch}-${suffix}.${ext} || true
mv mongo-debugsymbols.tgz mongodb-${push_name}-${push_arch}-debugsymbols-${suffix}.${ext} || true
mv distsrc.${ext} mongodb-src-${src_suffix}.${long_ext} || true

# generating checksums
function gen_checksums() {
  if [ -e $1 ]; then
    shasum -a 1 $1 | tee $1.sha1
    shasum -a 256 $1 | tee $1.sha256
    md5sum $1 | tee $1.md5
  else
    echo "$1 does not exist. Skipping checksum generation"
  fi
}

gen_checksums mongodb-$push_name-$push_arch-$suffix.$ext
gen_checksums mongodb-$push_name-$push_arch-debugsymbols-$suffix.$ext
gen_checksums mongodb-src-$src_suffix.$long_ext
gen_checksums mongodb-cryptd-$push_name-$push_arch-$suffix.$ext

# signing linux artifacts with gpg
cat << 'EOF' > gpg_signing_commands.sh
gpgloader # loading gpg keys.
function sign(){
  if [ -e $1 ]
  then
    gpg --yes -v --armor -o $1.sig --detach-sign $1
  else
    echo "$1 does not exist. Skipping signing"
  fi
}

EOF

cat << EOF >> gpg_signing_commands.sh
sign mongodb-$push_name-$push_arch-$suffix.$ext
sign mongodb-$push_name-$push_arch-debugsymbols-$suffix.$ext
sign mongodb-src-$src_suffix.$long_ext
sign mongodb-cryptd-$push_name-$push_arch-$suffix.$ext
EOF

podman run \
  -e GRS_CONFIG_USER1_USERNAME=${garasign_gpg_username_70} \
  -e GRS_CONFIG_USER1_PASSWORD=${garasign_gpg_password_70} \
  --rm \
  -v $(pwd):$(pwd) -w $(pwd) \
  ${garasign_gpg_image} \
  /bin/bash -c "$(cat ./gpg_signing_commands.sh)"
