cd src

set -o errexit
set -o verbose

tag="$future_git_tag"
echo "TAG: $tag"
if [[ ! -z $tag ]]; then
  if [ "Windows_NT" = "$OS" ]; then
    # On Windows, we don't seem to have a local git identity, so we populate the config with this
    # dummy email and name. Without a configured email/name, the 'git tag' command will fail.
    git config user.email "no-reply@evergreen.@mongodb.com"
    git config user.name "Evergreen Agent"
  fi

  git tag -a $tag -m $tag
fi
