cache_file=$1
basefile="${cache_file##*/}"
if [ "$basefile" != "content_hash" ]; then
  dir="${cache_file%/*}"
  ext="${dir##*.}"
  if [ "$ext" = "cksum" ]; then
    echo "Pruned $dir" >> scons_cache_prune.log
    rm -rf $dir
  else
    echo "Pruned $cache_file" >> scons_cache_prune.log
    rm -f $cache_file
  fi
fi
