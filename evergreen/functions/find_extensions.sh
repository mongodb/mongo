# Finds a specific extension if specified, otherwise lists all extension paths. Paths are outputted to a
# YML file named `extension_paths.yml` which the caller of the script can use as an expansion
# in their Evergreen configuration.
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "${DIR}/../prelude.sh"

cd src

# Check if a specific extension name is provided as an argument.
if [[ -n "${EXTENSION_NAME}" ]]; then
    echo "Finding specific extension: ${EXTENSION_NAME}"

    EXTENSION_PATH=$(find lib -name "${EXTENSION_NAME}")

    if [[ -z "${EXTENSION_PATH}" ]]; then
        echo "Error: Could not find the specified extension file: ${EXTENSION_NAME}"
        ls -R .
        exit 1
    fi

    echo "Found extension: ${EXTENSION_PATH}."
    echo "extension_paths: \"${EXTENSION_PATH}\"." >extension_paths.yml
else
    echo "EXTENSION_NAME not provided. Finding all unpacked extensions."
    # Find all .so files and create a comma-separated list.
    EXTENSIONS_LIST=$(find lib -name "*.so" | paste -sd, -)

    if [[ -z "${EXTENSIONS_LIST}" ]]; then
        echo "Error: Could not find any extracted extension files."
        ls -R .
        exit 1
    fi

    echo "Found extensions: ${EXTENSIONS_LIST}"
    echo "extension_paths: \"${EXTENSIONS_LIST}\"" >extension_paths.yml
fi
