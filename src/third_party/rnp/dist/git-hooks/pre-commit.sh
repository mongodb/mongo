#!/bin/bash
set -eEu

# Path to clang-format. If it's in your PATH, "clang-format" is fine.
# If you do not have it (or the version is too old), see USE_DOCKER.
CLANG_FORMAT="/usr/local/bin/clang-format"

# Set to "yes" if you want to create and use a docker container
# for clang-format.
USE_DOCKER="no"

# Set this to "yes" if you typically commit from the command line
# and wish to be prompted on whether to apply any patches
# automatically.
INTERACTIVE="yes"

# Leave this stuff
STASH_NAME="pre-commit-$(date +%s)"
CONTAINER_NAME="clang-format-$(date +%s)"
CLANG_FORMAT_VERSION="11.0.0"

apply_patch() {
  local patchfile=$1
  git apply --index "$patchfile"
  rm -f "$patchfile"
  exit 0
}

stash() {
  git stash save -q --keep-index "$STASH_NAME"
}

unstash() {
  if git stash list | head -n 1 | grep -Fq "$STASH_NAME"; then
    git stash pop -q || true
  fi
}

cleanup() {
  ec=$?
  unstash
  if [ $USE_DOCKER == "yes" ]; then
    docker kill "$CONTAINER_NAME" >/dev/null 2>&1 || true
  fi
  if [ $ec -ne 0 ]; then
    echo Aborted.
  fi
  exit $ec
}

if [ $USE_DOCKER == "yes" ]; then
  if [ ! "$(docker images clang-format --format '{{.Repository}}')" ]; then
    echo "Creating docker image for clang-format (one time only)..."
    docker run --name=clang-format alpine:latest apk --no-cache add clang >/dev/null 2>&1
    docker commit clang-format clang-format >/dev/null 2>&1
    docker rm clang-format
  fi
  CLANG_FORMAT="docker exec $CONTAINER_NAME clang-format"
  docker run --rm --name "$CONTAINER_NAME" -t -d -v "$(git rev-parse --show-toplevel)":/src -w /src clang-format tail -f /dev/null >/dev/null 2>&1
  # alternatively (slower for multiple files)
  #CLANG_FORMAT="docker run --rm -v $(git rev-parse --show-toplevel):/src -w /src clang-format clang-format"
fi

if git rev-parse --verify HEAD >/dev/null 2>&1
then
	against=HEAD
else
	# Initial commit: diff against an empty tree object
	against=4b825dc642cb6eb9a060e54bf8d69288fbee4904
fi

exec 1>&2

$CLANG_FORMAT -version | grep -Fq "$CLANG_FORMAT_VERSION" || (echo Incorrect version of clang-format.; exit 1)

patchfile=$(mktemp -t git-clang-format.XXXXXX.patch)
stash
trap "cleanup" SIGHUP SIGINT SIGTERM EXIT ERR

git diff-index --cached --diff-filter=ACMR --name-only $against -- | grep "\.[ch]$" | while read -r file;
do
  $CLANG_FORMAT -style=file "$file" |     \
    diff -u "$file" - |                     \
    sed -e "1s|--- |--- a/|" -e "2s|+++ -|+++ b/$file|" >> "$patchfile"

  # cat is just here to ignore the exit status of diff
  $CLANG_FORMAT -style=file "$file" | diff -u "$file" - | cat
done
unstash

if [ ! -s "$patchfile" ]; then
  rm -f "$patchfile"
  exit 0
fi

echo
echo Formatting changes requested.
echo "See $patchfile"
echo

if ! git apply --index --check "$patchfile"; then
  echo You may have unstaged changes to files that require formatting updates.
  echo It is not safe for this script to apply the patch automatically.
  exit 1
fi

if [ $INTERACTIVE == "yes" ]; then
  while true; do
    exec < /dev/tty
    read -rp "Apply now (y/n)? " yn
    exec <&-
    case $yn in
      [Yy]* ) apply_patch "$patchfile"; exit 0;;
      [Nn]* ) break;;
      * ) echo Please answer yes or no. ;;
    esac
  done
fi

echo
echo You should manually apply the patch with:
echo "git apply --index \"$patchfile\""
echo "Or use another method, and then restage."
exit 1

