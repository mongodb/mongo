DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/prelude.sh"

cd src

set -o errexit
set -o verbose

rm -rf ${install_directory}

# By default, limit link jobs to one quarter of our overall -j
# concurrency unless locally overridden. We do this because in
# static link environments, the memory consumption of each
# link job is so high that without constraining the number of
# links we are likely to OOM or thrash the machine. Dynamic
# builds, where htis is not a concern, override this value.
echo "Changing SCons to run with --jlink=${num_scons_link_jobs_available}"
extra_args="$extra_args --jlink=${num_scons_link_jobs_available} --separate-debug=${separate_debug}"

echo "Changing SCons to run with UNITTESTS_COMPILE_CONCURRENCY=${num_scons_unit_cc_jobs_available}"
extra_args="$extra_args UNITTESTS_COMPILE_CONCURRENCY=${num_scons_unit_cc_jobs_available}"

extra_args="$extra_args --consolidated-test-bins=on"

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

extra_args="$extra_args SPLIT_DWARF=0 GDB_INDEX=0 ENABLE_BUILD_RETRY=1 BAZEL_INTEGRATION_DEBUG=1"

echo "Setting evergreen tmp dir to $TMPDIR"
extra_args="$extra_args --evergreen-tmp-dir='${TMPDIR}'"

if [ "${generating_for_ninja}" = "true" ] && [ "Windows_NT" = "$OS" ]; then
  vcvars="$(vswhere -latest -property installationPath | tr '\\' '/' | dos2unix.exe)/VC/Auxiliary/Build/"
  export PATH="$(echo "$(cd "$vcvars" && cmd /C "vcvarsall.bat amd64 && C:/cygwin/bin/bash -c 'echo \$PATH'")" | tail -n +6)":$PATH
fi
activate_venv

# if build_patch_id is passed, try to download binaries from specified
# evergreen patch.
# This is purposfully before the venv setup so we do not touch the venv deps
build_patch_id="${build_patch_id:-${reuse_compile_from}}"
if [ -n "${build_patch_id}" ]; then
  echo "build_patch_id detected, trying to skip task"
  if [ "${task_name}" = "compile_dist_test" ] || [ "${task_name}" = "compile_dist_test_half" ]; then
    echo "Skipping ${task_name} compile without downloading any files"
    exit 0
  fi

  # On windows we change the extension to zip
  if [ -z "${ext}" ]; then
    ext="tgz"
  fi

  extra_db_contrib_args=""

  # get the platform of the dist archive. This is needed if
  # db-contrib-tool cannot autodetect the platform of the ec2 instance.
  regex='MONGO_DISTMOD=([a-z0-9]*)'
  if [[ ${compile_flags} =~ ${regex} ]]; then
    extra_db_contrib_args="${extra_db_contrib_args} --platform=${BASH_REMATCH[1]}"
  fi

  download_dir="./tmp_db_contrib_tool_download_dir"
  rm -rf ${download_dir}

  if [ "${task_name}" = "archive_dist_test" ]; then
    file_name="mongodb-binaries.${ext}"
    invocation="db-contrib-tool setup-repro-env ${build_patch_id} \
      --variant=${compile_variant} --extractDownloads=False \
      --binariesName=${file_name} --installDir=${download_dir} ${extra_db_contrib_args}"
  fi

  if [ "${task_name}" = "archive_dist_test_debug" ]; then
    file_name="mongo-debugsymbols.${ext}"
    invocation="db-contrib-tool setup-repro-env ${build_patch_id} \
      --variant=${compile_variant} --extractDownloads=False \
      --debugsymbolsName=${file_name} --installDir=${download_dir} \
      --skipBinaries --downloadSymbols ${extra_db_contrib_args}"
  fi

  if [ -n "${invocation}" ]; then
    setup_db_contrib_tool

    echo "db-contrib-tool invocation: ${invocation}"
    eval ${invocation}
    if [ $? -ne 0 ]; then
      echo "Could not retrieve files with db-contrib-tool"
      exit 1
    fi
    file_location=$(find "${download_dir}" -name "${file_name}")
    echo "Downloaded: ${file_location}"
    mv "${file_location}" "${file_name}"
    echo "Moved ${file_name} to the correct location"
    echo "Skipping ${task_name} compile"
    exit 0
  fi

  echo "Could not skip ${task_name} compile, compiling as normal"
fi

# --build-mongot is a compile flag used by the evergreen build variants that run end-to-end search
# suites, as it downloads the necessary mongot binary.
if [ "${build_mongot}" = "true" ]; then
  # Checking that this is not a downstream patch on mongod created by mongot's patch trigger.
  # In the case that it's not, download latest (eg HEAD of 10gen/mongot) or the
  # release (eg currently running in production on Atlas) mongot binary.
  if [[ ! $(declare -p linux_x86_64_mongot_localdev_binary linux_aarch64_mongot_localdev_binary macos_x86_64_mongot_localdev_binary 2> /dev/null) ]]; then
    if [ "${download_mongot_release}" = "true" ]; then
      extra_args="$extra_args --build-mongot=release"
    else
      extra_args="$extra_args --build-mongot=latest"
    fi
  else
    # This is a downstream patch, which means there is a patched mongot binary we need to install.
    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
      arch=$(uname -i)
      if [[ $arch == x86_64* ]]; then
        extra_args="$extra_args --patch-build-mongot-url=${linux_x86_64_mongot_localdev_binary}"
      elif [[ $arch == aarch64* ]]; then
        extra_args="$extra_args --patch-build-mongot-url=${linux_aarch64_mongot_localdev_binary}"
      else
        echo "mongot-localdev does not support ${arch}"
        exit 1
      fi
    elif [[ "$OSTYPE" == "darwin"* ]]; then
      extra_args="$extra_args --patch-build-mongot-url=${macos_x86_64_mongot_localdev_binary}"
    else
      echo "mongot-localdev does not support ${OSTYPE}"
      exit 1
    fi
  fi
fi

set -o pipefail

# Bind mount a new tmp directory to the real /tmp to circumvent "out of disk space" errors on ARM LTO compiles
# the /tmp directory is limited to 32 GB while the home directory has 500 GB
if [[ ${compile_flags} == *"--lto"* ]]; then
  set_sudo
  mkdir -p tmp && $sudo mount --bind ./tmp /tmp
fi

if [[ -n "${bazel_scons_diff_targets}" ]]; then
  diff_extra_args="--extra_args \"${compile_flags} ${task_compile_flags} ${task_compile_flags_extra} $extra_args\""
  eval ${compile_env} $python ./buildscripts/bazel_scons_diff.py \
    ${diff_extra_args} \
    ${bazel_scons_diff_targets}
fi

if [[ -n "${convert_bazel_headers_target}" ]]; then
  eval ${compile_env} $python ./buildscripts/convert_bazel_headers.py --target-library=${convert_bazel_headers_target} --silent > bazel_headers.txt
  exit 1
fi

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
