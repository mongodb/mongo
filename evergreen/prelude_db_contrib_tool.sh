function setup_db_contrib_tool {
    # check if db-contrib-tool is already installed
    if [[ -f "${workdir}/bin/db-contrib-tool" ]] || [[ -f "${workdir}/bin/db-contrib-tool.exe" ]]; then
        return 0
    fi

    $python evergreen/download_db_contrib_tool.py
}

function use_urpcli_versionset_mongot {
    # Scope 1 path: use urpcli versionset materialize to resolve the mongot artifact URL
    # from the versionless-staging version set, then download it directly.
    # Writes version_set.yaml to CWD as the task artifact.

    local urpcli_bin
    urpcli_bin="${workdir:-$HOME}/bin/urpcli"
    if [[ ! -f "${urpcli_bin}" ]]; then
        urpcli_bin="urpcli"
    fi

    echo "Materializing version set: urpcli versionset materialize versionless-staging"
    "${urpcli_bin}" versionset materialize versionless-staging

    if [[ ! -f "version_set.yaml" ]]; then
        echo "use_urpcli_versionset_mongot: version_set.yaml not written after materialize" >&2
        exit 1
    fi

    # Determine platform key used in version_set.yaml
    local vs_platform
    local arch
    arch=$(uname -m)
    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
        if [[ "${arch}" == "aarch64"* ]]; then
            vs_platform="linux-aarch64"
        else
            vs_platform="linux-x86_64"
        fi
    elif [[ "$OSTYPE" == "darwin"* ]]; then
        # macOS arm64 runs mongot under x86_64 emulation (Rosetta)
        vs_platform="macos-x86_64"
    else
        echo "use_urpcli_versionset_mongot: unsupported OS: ${OSTYPE}" >&2
        exit 1
    fi

    # Extract the mongot artifact URL for this platform from version_set.yaml using Python
    local mongot_url
    mongot_url=$(python3 - "${vs_platform}" <<'PYEOF'
import sys
import yaml

platform_key = sys.argv[1]

with open("version_set.yaml") as f:
    vs = yaml.safe_load(f)

# Expected structure:
#   artifacts:
#     search/mongot:
#       platforms:
#         linux-x86_64:
#           url: "https://..."
try:
    url = vs["artifacts"]["search/mongot"]["platforms"][platform_key]["url"]
    print(url)
except KeyError as e:
    sys.stderr.write(f"use_urpcli_versionset_mongot: key not found in version_set.yaml: {e}\n")
    sys.exit(1)
PYEOF
)

    if [[ -z "${mongot_url}" ]]; then
        echo "use_urpcli_versionset_mongot: could not resolve mongot URL for platform=${vs_platform}" >&2
        exit 1
    fi

    echo "Downloading mongot artifact: ${mongot_url}"
    mkdir -p ./mongot-localdev
    curl --retry 3 --retry-delay 5 --fail --show-error "${mongot_url}" | tar xvz -C ./mongot-localdev --strip-components=1
}

function use_db_contrib_tool_mongot {
    # Checking that this is not a downstream patch on mongod created by mongot's patch trigger.
    # In the case that it's not, download latest (eg HEAD of 10gen/mongot) or the
    # release (eg currently running in production on Atlas) mongot binary.
    arch=$(uname -i)
    if [[ ! $(declare -p linux_x86_64_mongot_localdev_binary linux_aarch64_mongot_localdev_binary macos_x86_64_mongot_localdev_binary 2>/dev/null) ]]; then

        # USE_URP_VERSION_SET=true opts into the Scope 1 urpcli-based path.
        # When not set, the existing db-contrib-tool behavior is preserved unchanged.
        if [[ "${USE_URP_VERSION_SET:-false}" == "true" ]]; then
            use_urpcli_versionset_mongot
            # Hack to remove BUILD.bazel file that can be lying around in mongot
            rm -f ./mongot-localdev/bin/jdk/BUILD.bazel
            return 0
        fi

        if [ "${download_mongot_release}" = "true" ]; then
            mongot_version="release"
        else
            mongot_version="latest"
        fi

        if [[ "$OSTYPE" == "linux-gnu"* ]]; then
            mongot_platform="linux"
        elif [[ "$OSTYPE" == "darwin"* ]]; then
            mongot_platform="macos"
        else
            echo "mongot is only supported on linux and mac and does not support ${OSTYPE}"
            exit 1
        fi

        mongot_arch="x86_64"
        # macos arm64 is not supported by mongot, but macos x86_64 runs on it successfully
        if [[ $arch == "aarch64"* ]] && [[ "$OSTYPE" != "darwin"* ]]; then
            mongot_arch="aarch64"
        fi
        echo "running: db-contrib-tool setup-mongot-repro-env ${mongot_version} --platform=${mongot_platform} --architecture=${mongot_arch} --installDir=."
        # This should create the folder mongot-localdev, usually run at the root of mongo directory
        db-contrib-tool setup-mongot-repro-env ${mongot_version} --platform=${mongot_platform} --architecture=${mongot_arch} --installDir=.
    else
        # This is a downstream patch, which means there is a patched mongot binary we need to install.
        if [[ "$OSTYPE" == "linux-gnu"* ]]; then
            if [[ $arch == "x86_64"* ]]; then
                mongot_url=${linux_x86_64_mongot_localdev_binary}
            elif [[ $arch == "aarch64"* ]]; then
                mongot_url=${linux_aarch64_mongot_localdev_binary}
            else
                echo "mongot-localdev does not support ${arch}"
                exit 1
            fi
        elif [[ "$OSTYPE" == "darwin"* ]]; then
            mongot_url=${macos_x86_64_mongot_localdev_binary}
        else
            echo "mongot-localdev does not support ${OSTYPE}"
            exit 1
        fi
        echo "running curl ${mongot_url} | tar xvz"
        # This should create the folder mongot-localdev, usually run at the root of mongo directory
        curl ${mongot_url} | tar xvz
    fi
    # Hack to remove BUILD.bazel file that can be lying around in mongot
    rm -f ./mongot-localdev/bin/jdk/BUILD.bazel
}
