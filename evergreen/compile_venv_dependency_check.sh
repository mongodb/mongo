# Quick check to ensure all scons.py dependecies have been added to compile-requirements.txt
set -o errexit
set -o verbose

# Create virtual env
/opt/mongodbtoolchain/v4/bin/virtualenv --python /opt/mongodbtoolchain/v4/bin/python3 ./compile_venv
source ./compile_venv/bin/activate

# Try printing scons.py help message
cd src
python -m pip install -r etc/pip/compile-requirements.txt
buildscripts/scons.py --help
