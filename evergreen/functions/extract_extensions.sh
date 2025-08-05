# This script extracts MongoDB extensions from a tarball. It's used in the Evergreen CI system to
# prepare extensions for testing. The script will skip extraction if extensions_required is not set
# to true.
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "${DIR}/../prelude.sh"

TARBALL="src/mongo-extensions.tgz"

if [[ "${extensions_required:-false}" != "true" ]]; then
    echo "Skipping task 'extract_extensions' because 'extensions_required' is not set to 'true'."
    exit 0

# Check if the tarball file exists.
elif [[ ! -f "${TARBALL}" ]]; then
    echo "ERROR: Tarball '${TARBALL}' not found."
    exit 1

# Check that the tarball file has a size greater than 0 bytes.
elif [[ ! -s "${TARBALL}" ]]; then
    echo "ERROR: Tarball '${TARBALL}' is empty."
    exit 1
fi

echo "Extracting extensions from ${TARBALL}..."
exec bash src/evergreen/run_python_script.sh "$@"
