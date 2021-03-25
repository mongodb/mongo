set -o errexit
set -o verbose

cd src
git clone --depth 1 git@github.com:10gen/mongo-enterprise-modules.git jstests/enterprise_tests
git clone --depth 1 git@github.com:10gen/QA.git jstests/qa_tests
