#!/bin/bash
# Builds aspio.jar on the host. Output: aspio.jar in the specified output directory.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
SRC_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# Defaults (overridable via env vars or CLI args)
ASPIO_DIR="${ASPIO_DIR:-$SRC_DIR/src/mongo/db/modules/enterprise/src/streams/aspio}"
TOOLS_DIR="${TOOLS_DIR:-$SRC_DIR/streams_build_tools}"
PROTOC_VERSION="${PROTOC_VERSION:-24.3}"
OUTPUT_DIR="${OUTPUT_DIR:-$SRC_DIR}"

usage() {
    cat <<EOF
Usage: $0 [OPTIONS]

Build aspio.jar (Maven + protoc)

OPTIONS:
    -a, --aspio-dir DIR           Path to aspio source directory
                                  (default: \$SRC_DIR/src/mongo/db/modules/enterprise/src/streams/aspio)
    -t, --tools-dir DIR           Directory to download build tools into
                                  (default: \$SRC_DIR/streams_build_tools)
    -p, --protoc-version VERSION  Protocol Buffers compiler version (default: 24.3)
    -o, --output-dir DIR          Directory to copy aspio.jar to (default: \$SRC_DIR)
    -h, --help                    Show this help message

ENVIRONMENT VARIABLES:
    ASPIO_DIR       Default aspio source directory if -a not specified
    TOOLS_DIR       Default tools directory if -t not specified
    PROTOC_VERSION  Default protoc version if -p not specified
    OUTPUT_DIR      Default output directory if -o not specified

EOF
    exit 0
}

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
    -a | --aspio-dir)
        if [[ -z "${2:-}" ]] || [[ "$2" =~ ^- ]]; then
            echo "Error: --aspio-dir requires a directory path"
            exit 1
        fi
        ASPIO_DIR="$2"
        shift 2
        ;;
    -t | --tools-dir)
        if [[ -z "${2:-}" ]] || [[ "$2" =~ ^- ]]; then
            echo "Error: --tools-dir requires a directory path"
            exit 1
        fi
        TOOLS_DIR="$2"
        shift 2
        ;;
    -p | --protoc-version)
        if [[ -z "${2:-}" ]] || [[ "$2" =~ ^- ]]; then
            echo "Error: --protoc-version requires a version string"
            exit 1
        fi
        PROTOC_VERSION="$2"
        shift 2
        ;;
    -o | --output-dir)
        if [[ -z "${2:-}" ]] || [[ "$2" =~ ^- ]]; then
            echo "Error: --output-dir requires a directory path"
            exit 1
        fi
        OUTPUT_DIR="$2"
        shift 2
        ;;
    -h | --help)
        usage
        ;;
    -*)
        echo "Error: Unknown option: $1"
        usage
        ;;
    *)
        echo "Error: Unexpected argument: $1"
        usage
        ;;
    esac
done

log "ASPIO_DIR: $ASPIO_DIR"
log "TOOLS_DIR: $TOOLS_DIR"
log "PROTOC_VERSION: $PROTOC_VERSION"
log "OUTPUT_DIR: $OUTPUT_DIR"

mkdir -p "$TOOLS_DIR"

if [ ! -d "$TOOLS_DIR/apache-maven-3.9.14" ]; then
    log "Downloading Maven 3.9.14..."
    wget -q https://archive.apache.org/dist/maven/maven-3/3.9.14/binaries/apache-maven-3.9.14-bin.zip -O "$TOOLS_DIR/maven.zip"
    unzip -q "$TOOLS_DIR/maven.zip" -d "$TOOLS_DIR"
    rm "$TOOLS_DIR/maven.zip"
fi
export MAVEN_HOME="$TOOLS_DIR/apache-maven-3.9.14"
export PATH="$MAVEN_HOME/bin:$PATH"

if [ ! -f "$TOOLS_DIR/bin/protoc" ]; then
    ARCH=$(uname -m | sed 's/aarch64/aarch_64/g' | sed 's/x86_64/x86_64/g')
    log "Downloading protoc ${PROTOC_VERSION} for ${ARCH}..."
    wget -q "https://github.com/protocolbuffers/protobuf/releases/download/v${PROTOC_VERSION}/protoc-${PROTOC_VERSION}-linux-${ARCH}.zip" -O "$TOOLS_DIR/protoc.zip"
    unzip -q "$TOOLS_DIR/protoc.zip" -d "$TOOLS_DIR"
    rm "$TOOLS_DIR/protoc.zip"
fi
export PATH="$TOOLS_DIR/bin:$PATH"

export LANG=C.UTF-8
cd "$ASPIO_DIR"
log "Running Maven build..."
mvn clean package -DskipTests

mkdir -p "$OUTPUT_DIR"
cp "$ASPIO_DIR/target/aspio.jar" "$OUTPUT_DIR/aspio.jar"
log "aspio.jar copied to $OUTPUT_DIR/aspio.jar"
