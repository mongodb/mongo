DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"
set -o errexit

# Writes ~/.netrc entries so Bazel can authenticate to GitHub for the private
# repos it fetches during a build.
# Required env vars:
#   GITHUB_MACHINES  Space-separated hosts to authenticate to, e.g.
#                    "github.com api.github.com"
#   GITHUB_TOKEN     Access token for the repos

: "${GITHUB_MACHINES:?}"
: "${GITHUB_TOKEN:?}"

netrc="${HOME}/.netrc"

for machine in ${GITHUB_MACHINES}; do
    # Drop any prior entry for this machine (e.g. an expired token) before
    # appending the current one, so at most one entry ever targets a given host.
    if [[ -f "$netrc" ]]; then
        awk -v m="$machine" '
            $1 == "machine" { skip = ($2 == m) }
            !skip { print }
        ' "$netrc" >"${netrc}.tmp"
        mv "${netrc}.tmp" "$netrc"
    fi

    {
        echo "machine ${machine}"
        echo "  login x-access-token"
        echo "  password ${GITHUB_TOKEN}"
    } >>"$netrc"
done
chmod 600 "$netrc"
