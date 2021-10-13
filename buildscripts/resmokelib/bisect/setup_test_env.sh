set -e
"$1" "$2" setup-multiversion -ec "$3" -db -da -i test_mongo -l test_mongo -v "$4" "$5"
mv test_mongo/"$5" test_mongo/mongo_repo
"$1" -m venv bisect_venv
source bisect_venv/bin/activate
"$1" -m pip install -r etc/pip/dev-requirements.txt
deactivate
