DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" > /dev/null 2>&1 && pwd)"
. "$DIR/../prelude.sh"

set +o errexit

cd src

if [ -z "${BOLT:-}" ]; then
  echo "Not applying BOLT" >&2
  exit 0
fi

tar -xvf mongodb-binaries.tgz
sudo rm mongodb-binaries.tgz

wget https://dsi-donot-remove.s3.us-west-2.amazonaws.com/llvm-bolt.gz
wget https://dsi-donot-remove.s3.us-west-2.amazonaws.com/bolt.data.gz
gunzip llvm-bolt.gz
sudo chmod +x llvm-bolt
gunzip bolt.data.gz

./llvm-bolt ./dist-test/bin/mongod -data bolt.data -reorder-blocks=ext-tsp -reorder-functions=hfsort -split-functions -split-all-cold -split-eh -dyno-stats --lite -o mongod_new --skip-funcs=_ZN8tcmalloc17tcmalloc_internal6subtle6percpu12TcmallocSlab4GrowEimmN4absl12lts_2023080211FunctionRefIFmhEEE,_ZN8tcmalloc17tcmalloc_internal18cpu_cache_internal8CpuCacheINS1_15StaticForwarderEE21DeallocateSlowNoHooksEPvm,_ZN8tcmalloc17tcmalloc_internal18cpu_cache_internal8CpuCacheINS1_15StaticForwarderEE19AllocateSlowNoHooksEm,calloc*,malloc*,_Znwm*,_ZnwmSt11align_val_t*,_ZdaPv*,realloc*,_ZdlPvmSt11align_val_t*,_ZdlPvm*,_ZN8tcmalloc17tcmalloc_internal6subtle6percpu12TcmallocSlab16CacheCpuSlabSlowEv,TcmallocSlab_Internal_PushBatch,TcmallocSlab_Internal_PopBatch

sudo rm dist-test/bin/mongod
cp mongod_new dist-test/bin/mongod

tar -czvf mongodb-binaries.tgz dist-test
