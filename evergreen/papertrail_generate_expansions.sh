cd src

set -o errexit
set -o verbose

version=$(git describe --tags --always --dirty)

if [ ${IS_RELEASE} = 'true' ]; then
  version="${version#r}"
fi

cat << EOT > papertrail-expansions.yml
release_version: "$version"
EOT
cat papertrail-expansions.yml
