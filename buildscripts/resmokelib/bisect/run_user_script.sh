set -e
cd build/resmoke-bisect
source bisect_venv/bin/activate
bash "$1"
deactivate
