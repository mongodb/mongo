DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

if [ "$(uname)" != "Linux" ] && [ "$(uname)" != "Darwin" ]; then
  echo "===== Skipping ulimit dump, OS is: $(uname)."
else
  echo "===== Collecting soft limits:"
  ulimit -Sa
  echo "===== Collecting hard limits:"
  ulimit -Ha
fi
