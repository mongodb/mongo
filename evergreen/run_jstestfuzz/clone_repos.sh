set -o errexit
set -o verbose

cd src
for i in {1..5}; do
  git clone --depth 1 git@github.com:10gen/QA.git jstests/qa_tests && RET=0 && break || RET=$? && sleep 5
  echo "Failed to clone git@github.com:10gen/QA.git, retrying..."
done

if [ $RET -ne 0 ]; then
  echo "Failed to clone git@github.com:10gen/QA.git"
  exit $RET
fi

cp -r src/mongo/db/modules/enterprise jstests/enterprise_tests
