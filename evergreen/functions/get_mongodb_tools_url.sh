get_mongodb_tools_url() {
    local arch=$(uname -m)
    local mongodb_tools_version="$1"
    local database_tools_url

    if [ -f /etc/os-release ]; then
        . /etc/os-release
        if [ "$ID" == "amzn" ]; then
            case $arch in
            "x86_64" | "aarch64")
                case $VERSION_ID in
                "2" | "2023")
                    database_tools_url="https://fastdl.mongodb.org/tools/db/mongodb-database-tools-amazon${VERSION_ID}-${arch}-${mongodb_tools_version}.tgz"
                    ;;
                *)
                    echo "Unsupported Amazon Linux version: $VERSION_ID"
                    return 1
                    ;;
                esac
                ;;
            *)
                echo "Unsupported architecture: $arch"
                return 1
                ;;
            esac
        else
            echo "Unsupported Linux distribution: $ID"
            return 1
        fi
    else
        echo "Unable to determine Linux distribution"
        return 1
    fi

    echo "$database_tools_url"
}
