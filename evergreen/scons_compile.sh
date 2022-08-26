DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

rm -rf ${install_directory}

# Use hardlinks to reduce the disk space impact of installing
# all of the binaries and associated debug info.

# The expansion here is a workaround to let us set a different install-action
# for tasks that don't support the one we set here. A better plan would be
# to support install-action for Ninja builds directly.
# TODO: https://jira.mongodb.org/browse/SERVER-48203
extra_args="--install-action=${task_install_action}"

# By default, limit link jobs to one quarter of our overall -j
# concurrency unless locally overridden. We do this because in
# static link environments, the memory consumption of each
# link job is so high that without constraining the number of
# links we are likely to OOM or thrash the machine. Dynamic
# builds, where htis is not a concern, override this value.
echo "Changing SCons to run with --jlink=${num_scons_link_jobs_available}"
extra_args="$extra_args --jlink=${num_scons_link_jobs_available} --separate-debug=${separate_debug}"

if [ "${scons_cache_scope}" = "shared" ]; then
  extra_args="$extra_args --cache-debug=scons_cache.log"
fi

# Conditionally enable scons time debugging
if [ "${show_scons_timings}" = "true" ]; then
  extra_args="$extra_args --debug=time,memory,count"
fi

# Build packages where the upload tasks expect them
if [ -n "${git_project_directory}" ]; then
  extra_args="$extra_args PKGDIR='${git_project_directory}'"
else
  extra_args="$extra_args PKGDIR='${workdir}/src'"
fi

# If we are doing a patch build or we are building a non-push
# build on the waterfall, then we don't need the --release
# flag. Otherwise, this is potentially a build that "leaves
# the building", so we do want that flag. The non --release
# case should auto enale the faster decider when
# applicable. Furthermore, for the non --release cases we can
# accelerate the build slightly for situations where we invoke
# SCons multiple times on the same machine by allowing SCons
# to assume that implicit dependencies are cacheable across
# runs.
if [ "${is_patch}" = "true" ] || [ -z "${push_bucket}" ] || [ "${compiling_for_test}" = "true" ]; then
  extra_args="$extra_args --implicit-cache --build-fast-and-loose=on"
else
  extra_args="$extra_args --release"
fi

extra_args="$extra_args SPLIT_DWARF=0"

if [ "${generating_for_ninja}" = "true" ] && [ "Windows_NT" = "$OS" ]; then
  vcvars="$(vswhere -latest -property installationPath | tr '\\' '/' | dos2unix.exe)/VC/Auxiliary/Build/"
  export PATH="$(echo "$(cd "$vcvars" && cmd /C "vcvarsall.bat amd64 && C:/cygwin/bin/bash -c 'echo \$PATH'")" | tail -n +6)":$PATH
fi
activate_venv

set -o pipefail
eval ${compile_env} $python ./buildscripts/scons.py \
  ${compile_flags} ${task_compile_flags} ${task_compile_flags_extra} \
  ${scons_cache_args} $extra_args \
  ${targets} MONGO_VERSION=${version} ${patch_compile_flags} | tee scons_stdout.log
exit_status=$?

# If compile fails we do not run any tests
if [[ $exit_status -ne 0 ]]; then
  touch ${skip_tests}
fi
exit $exit_status
