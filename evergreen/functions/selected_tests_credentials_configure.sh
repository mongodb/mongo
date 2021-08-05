DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

# Create the Evergreen API credentials
cat > .selected_tests.yml << END_OF_CREDS
url: "https://selected-tests.server-tig.prod.corp.mongodb.com"
project: "${project}"
auth_user: "${selected_tests_auth_user}"
auth_token: "${selected_tests_auth_token}"
END_OF_CREDS
