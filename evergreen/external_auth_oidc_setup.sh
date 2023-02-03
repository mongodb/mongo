DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit

# Should output contents to new file in home directory.
cat << EOF > $HOME/oidc_e2e_setup.json
{
    "testserversecurityone@okta-test.com" : "${oidc_okta_test_user_account_one_secret}",
    "testserversecuritytwo@okta-test.com" : "${oidc_okta_test_user_account_two_secret}",
    "testserversecuritythree@okta-test.com" : "${oidc_okta_test_user_account_three_secret}"
}
EOF
