DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

. ./notary_env.sh

set -o errexit
set -o verbose

ext="${ext:-tgz}"

mv "mongo_crypt_shared_v1.$ext" mongo_crypt_shared_v1-${push_name}-${push_arch}-${suffix}.${ext}

/usr/local/bin/notary-client.py \
  --key-name "server-6.0" \
  --auth-token-file ${workdir}/src/signing_auth_token \
  --comment "Evergreen Automatic Signing ${revision} - ${build_variant} - ${branch_name}" \
  --notary-url http://notary-service.build.10gen.cc:5000 \
  mongo_crypt_shared_v1-${push_name}-${push_arch}-${suffix}.${ext}
