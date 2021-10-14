set -e
"$1" "$2" setup-multiversion -ec "$3" -db -da -i build -l build -v "$4" "$5"
mv build/"$5" build/mongo_repo
"$1" -m venv build/bisect_venv
source build/bisect_venv/bin/activate
"$1" -m pip install --upgrade pip
"$1" -m pip install -r etc/pip/dev-requirements.txt
deactivate
