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
export extra_flags=""
eval ${compile_env} python3 ./buildscripts/scons.py ${compile_flags} $extra_flags --stack-size=1024 GITDIFFFLAGS="${revision}" REVISION="${revision}" ENTERPRISE_REV="${enterprise_rev}" ${targets} | tee scons-lint.log
exit_code=$?

$python ./buildscripts/simple_report.py --test-name "${targets}" --log-file scons-lint.log --exit-code $exit_code
exit $exit_code
