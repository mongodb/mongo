DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o pipefail
set -o verbose

activate_venv
export MYPY="$(
  if which cygpath 2> /dev/null; then
    PATH+=":$(cypath "${workdir}")/venv_3/Scripts"
  else
    PATH+=":${workdir}/venv_3/bin"
  fi
  PATH+=':/opt/mongodbtoolchain/v4/bin'
  which mypy
)"
echo "Found mypy executable at '$MYPY'"

echo "Setting evergreen tmp dir to $TMPDIR"
compile_flags="$compile_flags --evergreen-tmp-dir='${TMPDIR}'"

eval ${compile_env} python3 ./buildscripts/scons.py ${compile_flags} --stack-size=1024 ${targets} | tee scons-lint.log
exit_code=$?

$python ./buildscripts/simple_report.py --test-name "${targets}" --log-file scons-lint.log --exit-code $exit_code
exit $exit_code
