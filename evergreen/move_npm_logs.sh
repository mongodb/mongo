DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

set -o errexit
set -o verbose

if [ -d "${HOME}/.npm/_logs" ] && [ -n "$(ls -A "${HOME}/.npm/_logs")" ]; then
  cp -r "${HOME}"/.npm/_logs "${workdir}"
  rm -rf "${HOME}"/.npm/_logs/*
fi
