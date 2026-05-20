#!/bin/bash
# Runs spotless:check on the ASPIO Maven project to verify Java formatting.
# Fails if any file is not formatted according to Google Java Format.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

ASPIO_DIR="${ASPIO_DIR:-$SRC_DIR/src/mongo/db/modules/enterprise/src/streams/aspio}"
TOOLS_DIR="${TOOLS_DIR:-$SRC_DIR/streams_build_tools}"

# If you update this Java version, also update <java.version> in aspio/pom.xml and
# java-17-amazon-corretto-devel in evergreen/streams_image_build_and_push.sh and
# evergreen/streams_image_build_and_push_sanitizer.sh
sudo dnf -y install java-17-amazon-corretto-devel wget unzip

mkdir -p "$TOOLS_DIR"

# If you update this Maven version, also update it in evergreen/streams_build_aspio.sh
# (and update MAVEN_SHA512 below to match the new release's checksum from Apache)
if [ ! -d "$TOOLS_DIR/apache-maven-3.9.14" ]; then
    echo "Downloading Maven 3.9.14..."
    wget -q https://archive.apache.org/dist/maven/maven-3/3.9.14/binaries/apache-maven-3.9.14-bin.zip -O "$TOOLS_DIR/maven.zip"
    MAVEN_SHA512="4122c5e7a8794260539dd8fcd78480549511babff2f85e2b1258c8d4cf33c50af90f65d323f43c88d4959f35a8f37ced3eca802983caa6eb7cc81b16af936ab0"
    echo "$MAVEN_SHA512  $TOOLS_DIR/maven.zip" | sha512sum --check --quiet || {
        echo "Maven download checksum mismatch — aborting" >&2
        rm -f "$TOOLS_DIR/maven.zip"
        exit 1
    }
    unzip -q "$TOOLS_DIR/maven.zip" -d "$TOOLS_DIR"
    rm "$TOOLS_DIR/maven.zip"
fi
export MAVEN_HOME="$TOOLS_DIR/apache-maven-3.9.14"
export PATH="$MAVEN_HOME/bin:$PATH"

export LANG=C.UTF-8
cd "$ASPIO_DIR"
echo "Running spotless check in $ASPIO_DIR..."
mvn spotless:check
