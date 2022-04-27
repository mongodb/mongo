DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

. ./notary_env.sh

set -o errexit
set -o verbose

long_ext=${ext}
if [ "$long_ext" == "tgz" ]; then
  long_ext="tar.gz"
fi

if [[ "${push_name}" == "macos"* ]]; then
  curl https://macos-notary-1628249594.s3.amazonaws.com/releases/client/v3.3.0/linux_amd64.zip -o linux_amd64.zip
  unzip linux_amd64.zip
  chmod +x ./linux_amd64/macnotary
  bins=("mongo-binaries.tgz" "mongo-shell.tgz" "mongo-cryptd.tgz" "mh.tgz")
  for archive in ${bins[@]}; do
    if [ -f "$archive" ]; then
      TEMP_ARCHIVE="$(mktemp -p $PWD)"
      mv "$archive" "$TEMP_ARCHIVE"
      ./linux_amd64/macnotary -f "$TEMP_ARCHIVE" -m notarizeAndSign -u https://dev.macos-notary.build.10gen.cc/api -k server -s ${MACOS_NOTARY_TOKEN} -b server.mongodb.com -o "$archive"
      rm -f "$TEMP_ARCHIVE"
    else
      echo "Skipping macos notarization for $archive because it doesn't exist."
    fi
  done
fi

mv mongo-binaries.tgz mongodb-${push_name}-${push_arch}-${suffix}.${ext}
mv mongo-shell.tgz mongodb-shell-${push_name}-${push_arch}-${suffix}.${ext}
mv mongo-cryptd.tgz mongodb-cryptd-${push_name}-${push_arch}-${suffix}.${ext} || true
mv mh.tgz mh-${push_name}-${push_arch}-${suffix}.${ext} || true
mv mongo-debugsymbols.tgz mongodb-${push_name}-${push_arch}-debugsymbols-${suffix}.${ext} || true
mv distsrc.${ext} mongodb-src-${src_suffix}.${long_ext} || true
/usr/bin/find build/ -type f | grep msi$ | xargs -I original_filename cp original_filename mongodb-${push_name}-${push_arch}-${suffix}.msi || true

/usr/local/bin/notary-client.py --key-name "server-6.0" --auth-token-file ${workdir}/src/signing_auth_token --comment "Evergreen Automatic Signing ${revision} - ${build_variant} - ${branch_name}" --notary-url http://notary-service.build.10gen.cc:5000 --skip-missing mongodb-${push_name}-${push_arch}-${suffix}.${ext} mongodb-shell-${push_name}-${push_arch}-${suffix}.${ext} mongodb-${push_name}-${push_arch}-debugsymbols-${suffix}.${ext} mongodb-${push_name}-${push_arch}-${suffix}.msi mongodb-src-${src_suffix}.${long_ext} mongodb-cryptd-${push_name}-${push_arch}-${suffix}.${ext}
