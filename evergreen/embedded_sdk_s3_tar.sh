DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src/build

# Not using archive.targz_pack here because I can't get it to work.
set -o errexit
set -o verbose

cat << EOF > mongo-embedded-sdk-${version}/README-Licenses.txt
The software accompanying this file is Copyright (C) 2018 MongoDB, Inc. and
is licensed to you on the terms set forth in the following files:
  - mongo-c-driver: share/doc/mongo-c-driver/COPYING
  - mongo_embedded: share/doc/mongo_embedded/LICENSE-Embedded.txt
  - mongoc_embedded: share/doc/mongo_embedded/LICENSE-Embedded.txt
EOF

tar cfvz embedded-sdk.tgz mongo-embedded-sdk-${version}
