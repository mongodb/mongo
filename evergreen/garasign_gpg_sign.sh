set -o errexit
set -o verbose

cd src

mv mongo-binaries.tgz mongodb-${push_name}-${push_arch}-${suffix}.${ext|tgz}
mv mongo-shell.tgz mongodb-shell-${push_name}-${push_arch}-${suffix}.${ext|tgz}
mv mongo-cryptd.tgz mongodb-cryptd-${push_name}-${push_arch}-${suffix}.${ext|tgz} || true
mv mh.tgz mh-${push_name}-${push_arch}-${suffix}.${ext|tgz} || true
mv mongo-debugsymbols.tgz mongodb-${push_name}-${push_arch}-debugsymbols-${suffix}.${ext|tgz} || true
mv distsrc.${ext|tgz} mongodb-src-${src_suffix}.${ext|tar.gz} || true

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

gen_checksums mongodb-${push_name}-${push_arch}-${suffix}.${ext|tgz}
gen_checksums mongodb-shell-${push_name}-${push_arch}-${suffix}.${ext|tgz}
gen_checksums mongodb-cryptd-${push_name}-${push_arch}-${suffix}.${ext|tgz}
gen_checksums mh-${push_name}-${push_arch}-${suffix}.${ext|tgz}
gen_checksums mongodb-${push_name}-${push_arch}-debugsymbols-${suffix}.${ext|tgz}
gen_checksums mongodb-src-${src_suffix}.${ext|tar.gz}

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
sign mongodb-${push_name}-${push_arch}-${suffix}.${ext|tgz}
sign mongodb-shell-${push_name}-${push_arch}-${suffix}.${ext|tgz}
sign mongodb-cryptd-${push_name}-${push_arch}-${suffix}.${ext|tgz}
sign mh-${push_name}-${push_arch}-${suffix}.${ext|tgz}
sign mongodb-${push_name}-${push_arch}-debugsymbols-${suffix}.${ext|tgz}
sign mongodb-src-${src_suffix}.${ext|tar.gz}

EOF

podman run \
  -e GRS_CONFIG_USER1_USERNAME=${garasign_gpg_username_44} \
  -e GRS_CONFIG_USER1_PASSWORD=${garasign_gpg_password_44} \
  --rm \
  -v $(pwd):$(pwd) -w $(pwd) \
  ${garasign_gpg_image} \
  /bin/bash -c "$(cat ./gpg_signing_commands.sh)"
