DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

# Test exits from here with specified exit_code.
if [ -n "${exit_code}" ]; then
  # Python program saved exit_code
  exit_code=${exit_code}
elif [ -f error_exit.txt ]; then
  # Bash trap exit_code
  exit_code=$(cat error_exit.txt)
else
  exit_code=0
fi
echo "Exiting powercycle with code $exit_code"
exit $exit_code
