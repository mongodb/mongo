#!/bin/bash

set -o errexit

echo "+----------------------------------------------------------------------------+"
echo "|       Script to install the Endor Labs CLI and verify authentication       |"
echo "|              endorctl (https://docs.endorlabs.com/endorctl/)               |"
echo "| Environment Variables (optional):                                          |"
echo "|  ENDOR_INSTALL_PATH - only if in CI or not installed with homebrew or npm  |"
echo "|  ENDOR_CONFIG_PATH - endor config directory (default: ~/.endorctl)         |"
echo "+----------------------------------------------------------------------------+"
echo

function endorctl_check_install() {
    # Check if installed
    ENDOR_INSTALLED_PATH=$(command -v endorctl)
    if [[ -n "$ENDOR_INSTALLED_PATH" ]]; then
        # Is Installed
        echo "Binary 'endorctl' is installed in '${ENDOR_INSTALLED_PATH}'."
        chmod +x $ENDOR_INSTALLED_PATH
        if [[ -x "$ENDOR_INSTALLED_PATH" ]]; then
            echo "Binary 'endorctl' is executable."
            return 0 # True (success)
        else
            echo "Binary 'endorctl' is NOT executable after attempting to make it executable."
            return 1 # False (failure)
        fi
    else
        echo "Binary 'endorctl' is NOT installed or not in PATH."
        return 1 # False (failure)
    fi
}

function endorctl_install() {

    # Skip trying homebrew and npm if runing in CI
    if [[ "$CI" == "true" ]]; then
        echo "---------------------------------"
        echo "Detected that script is running in CI. Skipping Homebrew and NPM."
    else
        # Try brew
        echo "---------------------------------"
        echo "Checking if Homebrew is available"
        if command -v brew --version &>/dev/null; then
            echo "Attempting to install with Homebrew"
            brew tap endorlabs/tap
            brew install endorctl
            if [ $? -ne 0 ]; then
                echo "Warning: Homebrew installation failed."
            else
                echo "Installed with Homebrew"
                return 0 # True (success)
            fi
        else
            echo "Homebrew is not available"
        fi

        # Try NPM
        echo "---------------------------------"
        echo "Checking if npm is available"
        if command -v npm --version &>/dev/null; then
            # Install binary for linux or macos
            echo "Attempting to install with npm"
            npm install --global endorctl
            if [ $? -ne 0 ]; then
                echo "Warning: npm installation failed."
            else
                echo "Installed with npm"
                return 0 # True (success)
            fi
        else
            echo "npm is not available"
        fi
    fi

    # Try binary installation
    echo "---------------------------------"
    echo "Attempting binary install"

    if [[ -z "$ENDOR_INSTALL_PATH" ]]; then
        ENDOR_INSTALL_PATH="${HOME}/.local/bin"
    fi
    echo "Installation path set to $ENDOR_INSTALL_PATH"
    mkdir -p "$ENDOR_INSTALL_PATH"
    export PATH="${ENDOR_INSTALL_PATH}:$PATH"
    ENDOR_BIN_PATH="${ENDOR_INSTALL_PATH}/endorctl"

    case $(uname -m) in
    "x86_64" | "amd64")
        ARCH="amd64"
        ;;
    "aarch64" | "arm64")
        ARCH="arm64"
        ;;
    *)
        echo "Error: Unexpected architecture: $(uname -m). Expected x86_64, amd64, or arm64."
        return 1 # False (failure)
        ;;
    esac

    case "$OSTYPE" in
    linux*)
        PLATFORM="linux"
        ;;
    darwin*)
        PLATFORM="macos"
        ;;
    msys* | cygwin* | "Windows_NT")
        echo "Error: Automated installation on Windows without npm is not implemented in this script."
        echo "For manual Windows installation, follow instructions at:"
        echo "   https://docs.endorlabs.com/endorctl/install-and-configure/#download-and-install-the-endorctl-binary-directly"
        echo ""
        echo_auth_instructions
        return 1 # False (failure)
        ;;
    *)
        echo "Error: Unexpected OS type: $OSTYPE"
        return 1 # False (failure)
        ;;
    esac

    ## Download the latest CLI for supported platform and architecture
    URL="https://api.endorlabs.com/download/latest/endorctl_${PLATFORM}_${ARCH}"
    echo "Downloading latest CLI for $PLATFORM $ARCH to $BIN_PATH from $URL"
    curl --silent $URL --output "$ENDOR_BIN_PATH"
    ## Verify the checksum of the binary
    echo "Verifying checksum of binary"
    case "$PLATFORM" in
    linux)
        echo "$(curl -s https://api.endorlabs.com/sha/latest/endorctl_${PLATFORM}_${ARCH})" $ENDOR_BIN_PATH | sha256sum -c
        ;;
    macos)
        echo "$(curl -s https://api.endorlabs.com/sha/latest/endorctl_${PLATFORM}_${ARCH})" $ENDOR_BIN_PATH | shasum -a 256 -c
        ;;
    esac
    ## Modify the permissions of the binary to ensure it is executable
    echo "  Modifying binary permissions to executable"
    chmod +x $ENDOR_BIN_PATH
    ## Create an alias endorctl of the binary to ensure it is available in other directory
    alias endorctl=$ENDOR_BIN_PATH

    echo "endorctl installed in $ENDOR_BIN_PATH"
    return 0 # True (success)
}

function endorctl_check_auth() {
    # Check authentication
    echo "Checking authentication with command: endorctl api get --resource Project --namespace mongodb.10gen --name https://github.com/10gen/mongo.git --config-path $ENDOR_CONFIG_PATH"
    endorctl api get --resource Project --namespace mongodb.10gen --name "https://github.com/10gen/mongo.git" --config-path $ENDOR_CONFIG_PATH >/dev/null
    if [ $? -eq 0 ]; then
        echo "Authentication confirmed."
        return 0 # True (success)
    else
        echo "Authentication failure. Command exit code: $?"
        echo_auth_instructions
        return 1 # False (failure)
    fi
}

function echo_auth_instructions() {
    echo ""
    echo "------------------------------------------------ AUTOMATED AUTH ------------------------------------------------"
    echo "Set the following environment variables:"
    echo "   export ENDOR_API_CREDENTIALS_KEY=<api-key>"
    echo "   export ENDOR_API_CREDENTIALS_SECRET=<api-key-secret>"
    echo "   export ENDOR_NAMESPACE=mongodb.{github_org}"
    echo ""
    echo "--------------------------------------------------- USER AUTH ---------------------------------------------------"
    echo "To authenticate endorctl, visit the following URL, authenticate via Okta SSO, and copy the authentication token."
    echo "   https://api.endorlabs.com/v1/auth/sso?tenant=mongodb.10gen&redirect=headless"
    echo "Then run:"
    echo "   endorctl auth --token [AUTH_TOKEN]"
    echo ""
    echo "Alternatively, run the init command. Must use headless mode when no GUI is available:"
    echo "   endorctl init --auth-mode=sso --auth-tenant=mongodb.10gen --headless-mode"
    echo ""
    echo "Enter 'y' if prompted to overwrite existing configuration and/or delete account keys."
    echo ""
    echo "If authentication fails, confirm in MANA that you are a member of a '10gen-endor-labs-*' Okta group."
    echo ""
}

# Set/Create config folder
if [[ -z "$ENDOR_CONFIG_PATH" ]]; then
    ENDOR_CONFIG_PATH="${HOME}/.endorctl"
fi
echo "Config path set to ${ENDOR_CONFIG_PATH}"

if ! endorctl_check_install; then
    if ! endorctl_install; then
        exit 1
    fi
fi

if ! endorctl_check_auth; then
    exit 1
fi

exit 0
