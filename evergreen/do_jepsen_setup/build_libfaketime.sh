set -o errexit

cd src
# Build libfaketime. A version of libfaketime at least as new as v0.9.6-9-g75896bd is
# required to use the FAKETIME_NO_CACHE and FAKETIME_TIMESTAMP_FILE environment variables.
# Additionally, a version of libfaketime containing the changes mentioned in SERVER-29336
# is required to avoid needing to use libfaketimeMT.so.1 and serializing all calls to
# fake_clock_gettime() with a mutex.
git clone --branch=for-jepsen --depth=1 git@github.com:10gen/libfaketime.git
cd libfaketime
branch=$(git symbolic-ref --short HEAD)
commit=$(git show -s --pretty=format:"%h - %an, %ar: %s")
echo "Git branch: $branch, commit: $commit"
make PREFIX=$(pwd)/build/ LIBDIRNAME='.' install
