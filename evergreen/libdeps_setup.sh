DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

activate_venv

# Loop 5 times to retry libdeps install
# We have seen weird network errors that can sometimes mess up the pip install
# By retrying we would like to only see errors that happen consistently
for i in {1..5}; do
  python -m poetry install --no-root --sync -E libdeps && RET=0 && break || RET=$? && sleep 1
done

if [ $RET -ne 0 ]; then
  echo "Poetry install error for libdeps addition to venv"
  exit $RET
fi

cd ..

# Overwrite pip-requirements since this is installing additional requirements
python -m pip freeze > pip-requirements.txt
