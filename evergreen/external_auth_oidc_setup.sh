DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit

# Should output contents to new file in home directory.
cat <<EOF >$HOME/oidc_e2e_setup.json
{
    "testserversecurityone@ping-test.com" : "${oidc_ping_test_user_account_one_secret}",
    "testserversecuritytwo@ping-test.com" : "${oidc_ping_test_user_account_two_secret}",
    "testserversecuritythree@ping-test.com" : "${oidc_ping_test_user_account_three_secret}",
    "tD548GwE1@outlook.com" : "${oidc_azure_test_user_account_one_secret}",
    "tD548GwE2@outlook.com" : "${oidc_azure_test_user_account_two_secret}",
    "tD548GwE3@outlook.com" : "${oidc_azure_test_user_account_three_secret}",
    "testserversecurityone@okta-test.com" : "${oidc_okta_test_user_account_one_secret}",
    "testserversecuritytwo@okta-test.com" : "${oidc_okta_test_user_account_two_secret}",
    "testserversecuritythree@okta-test.com" : "${oidc_okta_test_user_account_three_secret}"
}
EOF
