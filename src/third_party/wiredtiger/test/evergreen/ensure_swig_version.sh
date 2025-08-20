# No shebang. This script is not for direct execution.

# Ensure the required SWIG version is available for builds.
#
# Note: It is important to source this script as opposite to
# executing it. Example:
#
#   source ./test/evergreen/ensure_swig_version.sh
#
# The script activates Python virtual environment and ensures
# the required SWIG version is available in $PATH.

SWIG_REQUIRED_VERSION="4.0.0"
SWIG_INSTALL_VERSION="4.2.1"
SWIG_VERSION=$(swig -version | awk '/SWIG Version/ {print $3}')

if [[ "$(printf '%s\n' "$SWIG_REQUIRED_VERSION" "$SWIG_VERSION" | \
         sort -V | head -n1)" == "$SWIG_REQUIRED_VERSION" ]]; then
    echo "SWIG version $SWIG_VERSION is $SWIG_REQUIRED_VERSION or later."
else
    echo "SWIG version $SWIG_VERSION is earlier than $SWIG_REQUIRED_VERSION. Installing a newer version $SWIG_INSTALL_VERSION ..."
    python3 -m venv venv
    source venv/bin/activate
    pip3 install swig==$SWIG_INSTALL_VERSION
    swig -version
fi
