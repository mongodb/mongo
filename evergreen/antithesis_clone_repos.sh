set -o errexit

# Clone the `QA` and `jstestfuzz` repos into the `workload` image build context.
#
# This runs immediately after `github.generate_token` rather than from inside resmoke's image
# builder. GitHub App installation tokens expire one hour after they are created, and the antithesis
# image build regularly runs longer than that before it reaches the point where these repos are
# needed -- cloning here keeps the token well within its lifetime. See SERVER-132667.
#
# Deliberately not `set -o verbose`: the clone URLs carry the tokens.

cd src

workload_build_context="buildscripts/antithesis/base_images/workload"

clone_repo() {
    repo=$1
    token=$2
    destination="$workload_build_context/$repo"

    if [ -d "$destination" ]; then
        echo "Found existing $repo repo at: $destination"
        return 0
    fi

    if [ -z "$token" ]; then
        echo "No token available for 10gen/$repo -- cannot clone."
        exit 1
    fi

    echo "Cloning 10gen/$repo to $destination..."
    for i in {1..5}; do
        git clone --depth 1 "https://x-access-token:${token}@github.com/10gen/${repo}.git" "$destination" && RET=0 && break || RET=$? && sleep 1
        echo "Failed to clone github.com/10gen/${repo}.git, retrying..."
    done

    if [ $RET -ne 0 ]; then
        echo "Failed to clone github.com/10gen/${repo}.git"
        exit $RET
    fi
}

clone_repo "QA" "${github_token_qa_temp}"
clone_repo "jstestfuzz" "${github_token_jstestfuzz_temp}"
