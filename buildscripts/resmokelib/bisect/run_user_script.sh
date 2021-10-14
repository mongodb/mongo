set -e
cd build
source bisect_venv/bin/activate
bash "$1"
deactivate
