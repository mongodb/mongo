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

mv mongo-binaries.tgz mongodb-${push_name}-${push_arch}-${suffix}.${ext}
mv mongo-shell.tgz mongodb-shell-${push_name}-${push_arch}-${suffix}.${ext}
mv mongo-cryptd.tgz mongodb-cryptd-${push_name}-${push_arch}-${suffix}.${ext} || true
mv mh.tgz mh-${push_name}-${push_arch}-${suffix}.${ext} || true
mv mongo-debugsymbols.tgz mongodb-${push_name}-${push_arch}-debugsymbols-${suffix}.${ext} || true
mv distsrc.${ext} mongodb-src-${src_suffix}.${long_ext} || true
/usr/bin/find build/ -type f | grep msi$ | xargs -I original_filename cp original_filename mongodb-${push_name}-${push_arch}-${suffix}.msi || true

/usr/local/bin/notary-client.py --key-name "server-5.0" --auth-token-file ${workdir}/src/signing_auth_token --comment "Evergreen Automatic Signing ${revision} - ${build_variant} - ${branch_name}" --notary-url http://notary-service.build.10gen.cc:5000 --skip-missing mongodb-${push_name}-${push_arch}-${suffix}.${ext} mongodb-shell-${push_name}-${push_arch}-${suffix}.${ext} mongodb-${push_name}-${push_arch}-debugsymbols-${suffix}.${ext} mongodb-${push_name}-${push_arch}-${suffix}.msi mongodb-src-${src_suffix}.${long_ext} mongodb-cryptd-${push_name}-${push_arch}-${suffix}.${ext}
