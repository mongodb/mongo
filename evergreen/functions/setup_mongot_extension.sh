# Setup script for mongot-extension on Evergreen.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o errexit
set -o verbose

CONF_DIR="${workdir}/tmp/mongo/extensions"
INSTALL_DIR="/usr/lib/mongo/extensions/mongot-extension"

# Detect architecture.
ARCH=$(uname -m)
if [ "$ARCH" = "aarch64" ]; then
    ARCH_SUFFIX="aarch64"
else
    ARCH_SUFFIX="x86_64"
fi

# The mongot-extension is only built for Amazon Linux 2 and Amazon Linux 2023.
PLATFORM=""
if [ -f /etc/os-release ]; then
    . /etc/os-release
    if [[ "$ID" == "amzn" ]]; then
        if [[ "$VERSION_ID" == "2023" ]]; then
            PLATFORM="amazon2023"
        elif [[ "$VERSION_ID" == "2" ]]; then
            PLATFORM="amazon2"
        fi
    fi
fi

if [ -z "$PLATFORM" ]; then
    echo "ERROR: Unsupported platform for mongot-extension."
    echo "The mongot-extension is only available for Amazon Linux 2 and Amazon Linux 2023."
    if [ -f /etc/os-release ]; then
        echo "Detected OS: $ID $VERSION_ID"
    else
        echo "Could not detect OS (no /etc/os-release)"
    fi
    exit 1
fi

EXTENSION_TARBALL_URL="https://mongot-extension.s3.amazonaws.com/latest/mongot-extension-latest-${PLATFORM}-${ARCH_SUFFIX}.tgz"

echo "=== Setting up mongot-extension ==="
echo "Detected platform: ${PLATFORM}"
echo "Detected architecture: ${ARCH_SUFFIX}"
echo "Downloading from: ${EXTENSION_TARBALL_URL}"
echo "Config directory: ${CONF_DIR}"
echo "Install directory: ${INSTALL_DIR}"

# Download the pre-built mongot-extension tarball from S3.
echo "Downloading mongot-extension..."
for i in {1..5}; do
    curl -fsSL -o mongot_extension.tgz "${EXTENSION_TARBALL_URL}" && RET=0 && break || RET=$? && sleep 5
    echo "Failed to download mongot-extension, retrying..."
done

if [ $RET -ne 0 ]; then
    echo "Failed to download mongot-extension from ${EXTENSION_TARBALL_URL}"
    exit $RET
fi

# Extract the tarball and install the extension.
echo "Extracting mongot-extension..."
tar -xzf mongot_extension.tgz

# The tarball contains mongot-extension.so.
EXTENSION_LIB="mongot-extension.so"
if [ ! -f "$EXTENSION_LIB" ]; then
    echo "ERROR: Could not find ${EXTENSION_LIB} in tarball"
    echo "Tarball contents:"
    tar -tzf mongot_extension.tgz
    exit 1
fi

echo "Found extension library: ${EXTENSION_LIB}"
echo "Installing mongot-extension to ${INSTALL_DIR}..."
sudo mkdir -p "${INSTALL_DIR}"
sudo cp "${EXTENSION_LIB}" "${INSTALL_DIR}/"

# Create mongot-extension.conf.
echo "Creating extension config..."
mkdir -p "${CONF_DIR}"
cat >"${CONF_DIR}/mongot-extension.conf" <<EOF
sharedLibraryPath: ${INSTALL_DIR}/mongot-extension.so
EOF

echo "=== mongot-extension setup complete ==="
echo "Extension installed to: ${INSTALL_DIR}/mongot-extension.so"
echo "Config file created at: ${CONF_DIR}/mongot-extension.conf"
