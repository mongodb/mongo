DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

activate_venv

valid_mongocryptd_variants=(
  "enterprise-amazon2"
  "enterprise-amazon2-arm64"
  "enterprise-amazon2-streams"
  "enterprise-amazon2023"
  "enterprise-amazon2023-arm64"
  "enterprise-debian12-64"
  "enterprise-linux-64-amazon-ami"
  "enterprise-macos"
  "enterprise-macos-arm64"
  "enterprise-rhel-81-ppc64le"
  "enterprise-rhel-8-64-bit"
  "enterprise-rhel-8-64-bit-coverage"
  "enterprise-rhel-8-64-bit-suggested"
  "enterprise-rhel-8-arm64"
  "enterprise-rhel-83-s390x"
  "enterprise-rhel-90-64-bit"
  "enterprise-rhel-90-arm64"
  "enterprise-rhel-93-64-bit"
  "enterprise-rhel-93-arm64"
  "enterprise-suse15-64"
  "enterprise-ubuntu2004-arm64"
  "enterprise-ubuntu2204-arm64"
  "enterprise-ubuntu2204-jepsen"
  "enterprise-ubuntu2404"
  "enterprise-ubuntu2404-arm64"
  "enterprise-ubuntu2004-64"
  "enterprise-ubuntu2204-64"
  "enterprise-ubuntu2404-64"
  "enterprise-windows"
  "enterprise-windows-debug-unoptimized"
  "enterprise-windows-inmem"
  "enterprise-windows-wtdevelop"
)

if [ $(find . -name mongocryptd${exe} | wc -l) -ge 1 ]; then
  echo "Validating that ${build_variant} is a known enterprise task for mongocryptd"
  for valid_mongocryptd_variant in "${valid_mongocryptd_variants[@]}"; do
    if [[ "$build_variant" == "$valid_mongocryptd_variant" ]]; then
      exit 0
    fi
  done
  echo "ERROR: ${build_variant} is not a known enterprise task for mongocryptd"
  exit 1
  # TODO(SERVER-100860): Fix validate_mongocryptd.py and re-enable it instead of the loop above
  # eval PATH=$PATH:$HOME $python ./buildscripts/validate_mongocryptd.py --variant "${build_variant}" etc/evergreen_yml_components/tasks/compile_tasks.yml
else
  echo "Skipping validation of ${build_variant} as the repository does not have a mongocryptd binary"
  exit 0
fi
