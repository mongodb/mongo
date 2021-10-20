set -e
source build/resmoke-bisect/bisect_venv/bin/activate
cd build/resmoke-bisect/"$1"
bash "$2"
deactivate
