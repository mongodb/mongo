DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

cd src

set -o errexit
set -o verbose
# removes files from the shared scons cache.

# Only prune on master branch
if [[ "${project}" == "mongodb-mongo-master" ]]; then

  set +o errexit

  if [ "Windows_NT" = "$OS" ]; then
    ./win_mount.sh
  else
    mount | grep "\/efs" > /dev/null
  fi
  if [ $? -eq 0 ]; then
    echo "Shared cache is already mounted"
  else
    echo "Shared cache - mounting file system"
    if [ "Windows_NT" = "$OS" ]; then
      ./win_mount.sh
    else
      sudo mount /efs
    fi
  fi
  set -o errexit

  if [ "Windows_NT" = "$OS" ]; then
    cache_folder=/cygdrive/x
  else
    cache_folder=/efs
  fi
  dirs=$(ls -l $cache_folder | grep -v total | awk '{print $NF}')

  echo "Pruning shared SCons directories"

  for dir in $dirs; do
    echo "Pruning $cache_folder/$dir/scons-cache"

    if [ -e $cache_folder/$dir/info/distro_name ]; then
      distro=$(cat $cache_folder/$dir/info/distro_name)
    fi

    # Set cache sizes by distro
    case $distro in
    ubuntu1604 | ubuntu1804 | rhel62 | rhel70)
      cache_size=600
      ;;
    *)
      # default
      cache_size=400
      ;;
    esac

    if [ "Windows_NT" = "$OS" ]; then
      echo "dir="$dir
      python buildscripts/scons_cache_prune.py --cache-dir x:/$dir/scons-cache --cache-size $cache_size --prune-ratio 1.0
    else
      sudo python buildscripts/scons_cache_prune.py --cache-dir /efs/$dir/scons-cache --cache-size $cache_size --prune-ratio 1.0
    fi
    echo ""
  done

  if [ "Windows_NT" = "$OS" ]; then
    net use X: /delete || true
  else
    sudo umount /efs || true
  fi

else
  echo "Not on master, shared SCons cache pruning skipped"
fi
