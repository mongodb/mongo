DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"
set -o errexit

# Scopes a GitHub access token to a single repo via git's url.insteadOf rewriting, rather than a
# blanket ~/.netrc entry for github.com -- so multiple private repos can each carry a differently
# scoped token without one clobbering another's credentials.
#
# Required env vars:
#   GITHUB_REPO_REMOTE  Full https remote URL, e.g. https://github.com/10gen/jstestfuzz.git
#   GITHUB_REPO_TOKEN   Access token for that repo

: "${GITHUB_REPO_REMOTE:?}"
: "${GITHUB_REPO_TOKEN:?}"

# Drop any insteadOf rewrite left over from a previous (e.g. now-expired) token for this same
# remote before adding the current one, so at most one rewrite ever targets a given remote.
while read -r key _; do
    [[ -n "$key" ]] && git config --global --unset-all "$key"
done < <(git config --global --get-regexp --fixed-value '^url\..*\.insteadof$' "$GITHUB_REPO_REMOTE" 2>/dev/null || true)

authed_remote="https://x-access-token:${GITHUB_REPO_TOKEN}@${GITHUB_REPO_REMOTE#https://}"
git config --global "url.${authed_remote}.insteadOf" "$GITHUB_REPO_REMOTE"
