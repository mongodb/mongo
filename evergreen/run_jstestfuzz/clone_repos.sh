set -o errexit
set -o verbose

cd src
git clone --depth 1 git@github.com:10gen/QA.git jstests/qa_tests
cp -r src/mongo/db/modules/enterprise jstests/enterprise_tests
