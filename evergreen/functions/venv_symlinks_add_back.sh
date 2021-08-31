DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

# Move to the parent of the Evergreen workdir, which is where the virtualenv is extracted to.
cd ..

set -o errexit
set -o verbose

# Ignore Windows since it seems to work.
if [ "Windows_NT" = "$OS" ]; then
  exit 0
fi

cd mongodb-mongo-venv/bin/

rm python
ln -s "$python" python

pythons=$(ls python3*)
for p in $pythons; do
  rm "$p"
  ln -s python "$p"
done
