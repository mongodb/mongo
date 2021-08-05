DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

# Create the Evergreen API credentials
cat > .evergreen.yml << END_OF_CREDS
api_server_host: https://evergreen.mongodb.com/api
api_key: "${evergreen_api_key}"
user: "${evergreen_api_user}"
END_OF_CREDS
