set -e
source bisect_venv/bin/activate
bash "$1"
deactivate
