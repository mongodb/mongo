# Finds a specific extension if specified, otherwise lists all extension paths. Paths are outputted to a
# YML file named `extension_paths.yml` which the caller of the script can use as an expansion
# in their Evergreen configuration.
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "${DIR}/../prelude.sh"

# DIR = src/evergreen/functions/
TOP_LEVEL_DIR="${DIR}/../../.."

# Add extension_paths.yml as the expansion.update command will fail if the file does not exist.
touch "${TOP_LEVEL_DIR}/extension_paths.yml"

if [[ "$(uname -s)" != "Linux" || "${skip_extensions:-false}" == "true" ]]; then
    echo "Skipping task 'find_extensions'."
    exit 0
fi

LIB_SRC="${TOP_LEVEL_DIR}/src/dist-test/lib"

# Check if a specific extension name is provided as an argument.
if [[ -n "${EXTENSION_NAME}" ]]; then
    echo "Finding specific extension: ${EXTENSION_NAME}."

    EXTENSION_PATH=$(find "${LIB_SRC}" -name "${EXTENSION_NAME}")

    if [[ -z "${EXTENSION_PATH}" ]]; then
        echo "Error: Could not find the specified extension file: ${EXTENSION_NAME}."
        exit 1
    fi

    echo "Found extension: ${EXTENSION_PATH}."
    echo "extension_paths: \"${EXTENSION_PATH}\"" >"${TOP_LEVEL_DIR}/extension_paths.yml"
else
    echo "EXTENSION_NAME not provided. Finding all unpacked extensions."
    # Find all *_mongo_extension.so files and create a comma-separated list.
    EXTENSIONS_LIST=$(find "${LIB_SRC}" -name "*_mongo_extension.so" | paste -sd, -)

    if [[ -z "${EXTENSIONS_LIST}" ]]; then
        echo "Error: Could not find any extracted extension files."
        exit 1
    fi

    echo "Found extensions: ${EXTENSIONS_LIST}."
    echo "extension_paths: \"${EXTENSIONS_LIST}\"" >"${TOP_LEVEL_DIR}/extension_paths.yml"
fi
