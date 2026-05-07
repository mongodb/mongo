cd src

set -o errexit
set -o verbose

tag=""
if [ -n "$bv_future_git_tag" ]; then
    tag="$bv_future_git_tag"
fi
if [ -n "$future_git_tag" ]; then
    tag="$future_git_tag"
fi

echo "TAG: $tag"

if [ -n "$tag" ]; then
    if [ "Windows_NT" = "$OS" ]; then
        # On Windows, we don't seem to have a local git identity, so we populate the config with this
        # dummy email and name. Without a configured email/name, the 'git tag' command will fail.
        git config user.email "no-reply@evergreen.@mongodb.com"
        git config user.name "Evergreen Agent"
    fi

    git tag -a "$tag" -m "$tag"

    # Write the tag version into .bazelrc.target_mongo_version so the build
    # picks it up (version_expansions_generate.sh reads this file, not git describe).
    version="${tag#r}"
    echo "common --define=MONGO_VERSION=${version}" >.bazelrc.target_mongo_version
fi
