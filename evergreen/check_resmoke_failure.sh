set -o verbose
cd src
if [ -f resmoke_error_code ]; then
  exit $(cat resmoke_error_code)
fi
