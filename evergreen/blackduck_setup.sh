DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

# Create the Evergreen API credentials
cat > .restconfig.json << END_OF_CREDS
{
"baseurl": "${blackduck_url}",
"username": "${blackduck_username}",
"password": "${blackduck_password}",
"debug": false,
"insecure" : false
}
END_OF_CREDS
