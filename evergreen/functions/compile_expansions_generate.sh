DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o errexit
set -o verbose
# We get the raw version string (r1.2.3-45-gabcdef) from git
MONGO_VERSION=$(git describe --abbrev=7)
# If this is a patch build, we add the patch version id to the version string so we know
# this build was a patch, and which evergreen task it came from
if [ "${is_patch}" = "true" ]; then
  MONGO_VERSION="$MONGO_VERSION-patch-${version_id}"
fi
echo "MONGO_VERSION = ${MONGO_VERSION}"
activate_venv
# shared scons cache testing
# if 'scons_cache_scope' enabled and project level 'disable_shared_scons_cache' is not true
# 'scons_cache_scope' is set on a per variant basis
# 'disable_shared_scons_cache' is set on a project level and applies to all variants
# Shared - if scons_cache_scope is set, then use new shared scons cache settings
if [ ! -z ${scons_cache_scope} ]; then
  if [ "${disable_shared_scons_cache}" = "true" ]; then
    echo "SCons Cache disabled. All shared scons settings will be ignored"
    scons_cache_scope=none
  else
    scons_cache_scope=${scons_cache_scope}
  fi
  if [ "$scons_cache_scope" = "shared" ]; then
    set +o errexit
    if [ "Windows_NT" = "$OS" ]; then
      ./win_mount.sh
    else
      mount | grep "\/efs" > /dev/null
      if [ $? -eq 0 ]; then
        echo "Shared cache is already mounted"
      else
        echo "Shared cache - mounting file system"
        set_sudo
        $sudo mount /efs
      fi
    fi
    set -o errexit
  fi
  echo "Shared Cache with setting: ${scons_cache_scope}"
  MONGO_VERSION=$MONGO_VERSION SCONS_CACHE_MODE=${scons_cache_mode} SCONS_CACHE_SCOPE=$scons_cache_scope IS_PATCH=${is_patch} IS_COMMIT_QUEUE=${is_commit_queue} $python buildscripts/generate_compile_expansions_shared_cache.py --out compile_expansions.yml
# Legacy Expansion generation
else
  echo "Using legacy expansion generation"
  # Proceed with regular expansions generated
  # This script converts the generated version string into a sanitized version string for
  # use by scons and uploading artifacts as well as information about for the scons cache.
  MONGO_VERSION=$MONGO_VERSION SCONS_CACHE_MODE=${scons_cache_mode} USE_SCONS_CACHE=${use_scons_cache} $python buildscripts/generate_compile_expansions.py --out compile_expansions.yml
fi
