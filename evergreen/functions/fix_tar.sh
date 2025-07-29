DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

set +o errexit

cd src

# For whatever reason, signing on mac doesn't like the tar file produced from bazel
# Round trip the tar by un-taring then re-taring it
if [[ "$OSTYPE" == "darwin"* ]]; then
  mkdir ./tmp-tar
  tar -xvf $1 -C ./tmp-tar
  sudo rm $1
  tar -czvf $1 -C ./tmp-tar .
  sudo rm -rf ./tmp-tar
fi
