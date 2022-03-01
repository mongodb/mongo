#!/bin/bash

set -o errexit -o verbose

rm -rf extract include

mkdir extract
mkdir extract/js

# copy ICU
cp -R mozilla-release/intl extract/

cd mozilla-release/js/src

echo "Run autoconf2.13"
if [[ -x $AUTOCONF_LEGACY ]]; then
  $AUTOCONF_LEGACY
elif which autoconf213; then
  autoconf213
elif which autoconf2.13; then
  autoconf2.13
elif which autoconf-2.13; then
  autoconf-2.13
else
  echo 'Failed to find autoconf 2.13. Please specify its path in $AUTOCONF_LEGACY.'
  exit 1
fi

echo "Create _build if necessary"
rm -rf _build
mkdir -p _build
cd _build

rm config.cache || true

echo "Run configure"
# The 'ppc64le/linux' platform requires the additional 'CXXFLAGS' and 'CFLAGS' flags to compile
CXXFLAGS="$CXXFLAGS -D__STDC_FORMAT_MACROS" \
CFLAGS="$CFLAGS -D__STDC_FORMAT_MACROS" \
../configure \
    --disable-jemalloc \
    --with-system-zlib \
    --without-intl-api \
    --enable-optimize \
    --disable-js-shell \
    --disable-tests


# we have to run make to generate a byte code version of the self hosted js and
# a switch table
echo "Run make"
make

cd ../../../..

cp -r mozilla-release/mfbt extract/

cp -r mozilla-release/js/src mozilla-release/js/public extract/js/

cp mozilla-release/js/src/_build/js/src/selfhosted.out.h extract/js/src
mkdir -p extract/js/src/frontend
cp mozilla-release/js/src/_build/js/src/frontend/ReservedWordsGenerated.h extract/js/src/frontend

mkdir -p include/gc
cp extract/js/src/_build/js/src/gc/StatsPhasesGenerated.h include/gc
mkdir -p extract/js/src/gc
cp extract/js/src/_build/js/src/gc/StatsPhasesGenerated.inc extract/js/src/gc

# mfbt doesn't change by arch or platform, so keep the same unified cpp
mkdir -p extract/js/src/mfbt

find ./mozilla-release/js/src/_build/mfbt -name "Unified_cpp_mfbt*.cpp" -exec cp '{}' extract/js/src/mfbt ';'

SEDOPTION="-i"
if [[ "$OSTYPE" == "darwin"* ]]; then
  SEDOPTION="-i ''"
fi

for unified_file in extract/js/src/mfbt/*.cpp ; do
    echo "Processing $unified_file"
    sed $SEDOPTION \
        -e 's|#include ".*/mfbt/|#include "|' \
        -e 's|#error ".*/mfbt/|#error "|' \
        "$unified_file"
done


# stuff we can toss
rm -rf \
    extract/js/src/_build/backend.mk \
    extract/js/src/_build/backend.RecursiveMakeBackend \
    extract/js/src/_build/_build_manifests \
    extract/js/src/_build/config \
    extract/js/src/_build/config.cache \
    extract/js/src/_build/config.log \
    extract/js/src/_build/config.status \
    extract/js/src/configure \
    extract/js/src/configure.in \
    extract/js/src/ctypes \
    extract/js/src/doc \
    extract/js/src/editline \
    extract/js/src/gdb \
    extract/js/src/jit-test \
    extract/js/src/jsapi-tests \
    extract/js/src/_build/Makefile \
    extract/js/src/Makefile.in \
    extract/js/src/make-source-package.sh \
    extract/js/src/octane \
    extract/js/src/_build/python \
    extract/js/src/README.html \
    extract/js/src/_build/root-deps.mk \
    extract/js/src/_build/root.mk \
    extract/js/src/shell \
    extract/js/src/tests \
    extract/js/src/_build/_virtualenvs

# stuff we don't want to deal with due to licensing
rm -rf \
    extract/mfbt/tests \
    extract/js/src/util/make_unicode.py \
    extract/js/src/vtune

# this is all of the EXPORTS files from the moz.build
mkdir -p include
for i in 'jsapi.h' 'jsfriendapi.h' 'jspubtd.h' 'jstypes.h' ; do
    cp extract/js/src/$i include
done

# this is all of the EXPORTS.js files from the moz.build
mkdir -p include/js
for i in 'AllocationLogging.h' 'AllocationRecording.h' 'AllocPolicy.h' 'Array.h' 'ArrayBuffer.h' 'ArrayBufferMaybeShared.h' 'BigInt.h' 'BuildId.h' 'CallArgs.h' 'CallNonGenericMethod.h' 'CharacterEncoding.h' 'Class.h' 'ComparisonOperators.h' 'CompilationAndEvaluation.h' 'CompileOptions.h' 'Context.h' 'ContextOptions.h' 'Conversions.h' 'Date.h' 'Debug.h' 'Equality.h' 'ErrorReport.h' 'Exception.h' 'ForOfIterator.h' 'GCAnnotations.h' 'GCAPI.h' 'GCHashTable.h' 'GCPolicyAPI.h' 'GCTypeMacros.h' 'GCVariant.h' 'GCVector.h' 'HashTable.h' 'HeapAPI.h' 'HelperThreadAPI.h' 'Id.h' 'Initialization.h' 'JSON.h' 'LocaleSensitive.h' 'MapAndSet.h' 'MemoryFunctions.h' 'MemoryMetrics.h' 'Modules.h' 'Object.h' 'OffThreadScriptCompilation.h' 'Principals.h' 'Printf.h' 'ProfilingCategory.h' 'ProfilingFrameIterator.h' 'ProfilingStack.h' 'Promise.h' 'PropertyDescriptor.h' 'PropertySpec.h' 'ProtoKey.h' 'Proxy.h' 'Realm.h' 'RealmIterators.h' 'RealmOptions.h' 'RefCounted.h' 'RegExp.h' 'RegExpFlags.h' 'Result.h' 'RootingAPI.h' 'SavedFrameAPI.h' 'ScalarType.h' 'SharedArrayBuffer.h' 'SliceBudget.h' 'SourceText.h' 'StableStringChars.h' 'Stream.h' 'String.h' 'StructuredClone.h' 'SweepingAPI.h' 'Symbol.h' 'TraceKind.h' 'TraceLoggerAPI.h' 'TracingAPI.h' 'Transcoding.h' 'TypeDecls.h' 'UbiNode.h' 'UbiNodeBreadthFirst.h' 'UbiNodeCensus.h' 'UbiNodeDominatorTree.h' 'UbiNodePostOrder.h' 'UbiNodeShortestPaths.h' 'UbiNodeUtils.h' 'UniquePtr.h' 'Utility.h' 'Value.h' 'ValueArray.h' 'Vector.h' 'Warnings.h' 'WasmFeatures.h' 'WasmModule.h' 'WeakMap.h' 'WeakMapPtr.h' 'Wrapper.h' 'Zone.h' ; do
    cp extract/js/public/$i include/js/
done

for i in 'ProfilingCategoryList.h' ; do
    cp mozilla-release/mozglue/baseprofiler/public/$i include/js/
done

# this is all of the EXPORTS.js.experimental files from the moz.build
mkdir -p include/js/experimental
for i in 'CodeCoverage.h' 'CTypes.h' 'Intl.h' 'JitInfo.h' 'JSStencil.h' 'PCCountProfiling.h' 'SourceHook.h' 'TypedData.h' ; do
    cp extract/js/public/experimental/$i include/js/experimental/
done

# this is all of the EXPORTS.js.friend files from the moz.build
mkdir -p include/js/friend
for i in 'DOMProxy.h' 'DumpFunctions.h' 'ErrorMessages.h' 'ErrorNumbers.msg' 'JSMEnvironment.h' 'PerformanceHint.h' 'StackLimits.h' 'UsageStatistics.h' 'WindowProxy.h' 'XrayJitInfo.h' ; do
    cp extract/js/public/friend/$i include/js/friend/
done

# this is all of the EXPORTS.js.shadow files from the moz.build
mkdir -p include/js/shadow
for i in 'Function.h' 'Object.h' 'Realm.h' 'Shape.h' 'String.h' 'Symbol.h' 'Zone.h' ; do
    cp extract/js/public/shadow/$i include/js/shadow/
done

# this is all of the EXPORTS.mozilla files from the moz.build's
mkdir -p include/mozilla
for i in 'Algorithm.h' 'Alignment.h' 'AllocPolicy.h' 'AlreadyAddRefed.h' 'Array.h' 'ArrayUtils.h' 'Assertions.h' 'AtomicBitfields.h' 'Atomics.h' 'Attributes.h' 'BinarySearch.h' 'BitSet.h' 'BloomFilter.h' 'Buffer.h' 'BufferList.h' 'Casting.h' 'ChaosMode.h' 'Char16.h' 'CheckedInt.h' 'CompactPair.h' 'Compiler.h' 'Compression.h' 'DbgMacro.h' 'DebugOnly.h' 'DefineEnum.h' 'DoublyLinkedList.h' 'EndianUtils.h' 'EnumeratedArray.h' 'EnumeratedRange.h' 'EnumSet.h' 'EnumTypeTraits.h' 'fallible.h' 'FastBernoulliTrial.h' 'FloatingPoint.h' 'FStream.h' 'FunctionRef.h' 'FunctionTypeTraits.h' 'HashFunctions.h' 'HashTable.h' 'HelperMacros.h' 'InitializedOnce.h' 'IntegerRange.h' 'IntegerTypeTraits.h' 'JSONWriter.h' 'JsRust.h' 'Latin1.h' 'Likely.h' 'LinkedList.h' 'MacroArgs.h' 'MacroForEach.h' 'MathAlgorithms.h' 'Maybe.h' 'MaybeOneOf.h' 'MaybeStorageBase.h' 'MemoryChecking.h' 'MemoryReporting.h' 'NonDereferenceable.h' 'NotNull.h' 'Opaque.h' 'OperatorNewExtensions.h' 'PairHash.h' 'Path.h' 'PodOperations.h' 'Poison.h' 'RandomNum.h' 'Range.h' 'RangedArray.h' 'RangedPtr.h' 'ReentrancyGuard.h' 'RefCounted.h' 'RefCountType.h' 'RefPtr.h' 'Result.h' 'ResultExtensions.h' 'ResultVariant.h' 'ReverseIterator.h' 'RollingMean.h' 'Saturate.h' 'Scoped.h' 'ScopeExit.h' 'SegmentedVector.h' 'SHA1.h' 'SharedLibrary.h' 'SmallPointerArray.h' 'Span.h' 'SplayTree.h' 'SPSCQueue.h' 'StaticAnalysisFunctions.h' 'TaggedAnonymousMemory.h' 'Tainting.h' 'TemplateLib.h' 'TextUtils.h' 'ThreadLocal.h' 'ThreadSafeWeakPtr.h' 'ToString.h' 'Tuple.h' 'TypedEnumBits.h' 'Types.h' 'TypeTraits.h' 'UniquePtr.h' 'UniquePtrExtensions.h' 'Unused.h' 'Utf8.h' 'Variant.h' 'Vector.h' 'WeakPtr.h' 'WindowsVersion.h' 'WrappingOperations.h' 'XorShift128PlusRNG.h' ; do
    cp extract/mfbt/$i include/mozilla/
done

# this is all of the EXPORTS.mozilla.intl files from the moz.build's
mkdir -p include/mozilla/intl
for i in 'Calendar.h' 'DateTimeFormat.h' 'DateTimePatternGenerator.h' 'ICU4CGlue.h' 'NumberFormat.h' 'PluralRules.h' ; do
    cp extract/intl/components/src/$i include/mozilla/intl/
done

# this is all of the EXPORTS.unicode files from the moz.build's
mkdir -p include/unicode
for i in 'alphaindex.h' 'basictz.h' 'calendar.h' 'choicfmt.h' 'coleitr.h' 'coll.h' 'compactdecimalformat.h' 'curramt.h' 'currpinf.h' 'currunit.h' 'datefmt.h' 'dcfmtsym.h' 'decimfmt.h' 'dtfmtsym.h' 'dtitvfmt.h' 'dtitvinf.h' 'dtptngen.h' 'dtrule.h' 'fieldpos.h' 'fmtable.h' 'format.h' 'formattedvalue.h' 'fpositer.h' 'gender.h' 'gregocal.h' 'listformatter.h' 'measfmt.h' 'measunit.h' 'measure.h' 'msgfmt.h' 'nounit.h' 'numberformatter.h' 'numberrangeformatter.h' 'numfmt.h' 'numsys.h' 'plurfmt.h' 'plurrule.h' 'rbnf.h' 'rbtz.h' 'regex.h' 'region.h' 'reldatefmt.h' 'scientificnumberformatter.h' 'search.h' 'selfmt.h' 'simpletz.h' 'smpdtfmt.h' 'sortkey.h' 'stsearch.h' 'tblcoll.h' 'timezone.h' 'tmunit.h' 'tmutamt.h' 'tmutfmt.h' 'translit.h' 'tzfmt.h' 'tznames.h' 'tzrule.h' 'tztrans.h' 'ucal.h' 'ucol.h' 'ucoleitr.h' 'ucsdet.h' 'udat.h' 'udateintervalformat.h' 'udatpg.h' 'ufieldpositer.h' 'uformattable.h' 'uformattedvalue.h' 'ugender.h' 'ulistformatter.h' 'ulocdata.h' 'umsg.h' 'unirepl.h' 'unum.h' 'unumberformatter.h' 'unumberrangeformatter.h' 'unumsys.h' 'upluralrules.h' 'uregex.h' 'uregion.h' 'ureldatefmt.h' 'usearch.h' 'uspoof.h' 'utmscale.h' 'utrans.h' 'vtzone.h' ; do
    cp extract/intl/icu/source/i18n/unicode/$i include/unicode/
done

for i in 'malloc_decls.h' 'mozjemalloc_types.h' 'mozmemory.h' 'mozmemory_wrap.h' ; do
    cp extract/js/src/_build/dist/include/$i include
done

mkdir -p include/double-conversion
cp -r extract/mfbt/double-conversion/double-conversion/*.h include/double-conversion

for i in 'AutoProfilerLabel.h' 'PlatformConditionVariable.h' 'PlatformMutex.h' 'Printf.h' 'StackWalk.h' 'TimeStamp.h' 'fallible.h' ; do
    cp extract/js/src/_build/dist/include/mozilla/$i include/mozilla
done

cp extract/js/src/_build/dist/include/fdlibm.h include
mkdir -p extract/modules/fdlibm
cp mozilla-release/modules/fdlibm/src/* extract/modules/fdlibm/

cp mozilla-release/mozglue/misc/*.h include
cp mozilla-release/mozglue/misc/*.h include/mozilla
mkdir -p extract/mozglue/misc
cp mozilla-release/mozglue/misc/*.cpp extract/mozglue/misc
cp mozilla-release/mozglue/misc/StackWalk_windows.h include/mozilla/

mkdir -p include/vtune
touch include/vtune/VTuneWrapper.h

xargs rm -r<<__XARGS_RM__
extract/js/src/_build/backend.FasterMakeBackend.in
extract/js/src/_build/backend.RecursiveMakeBackend.in
extract/js/src/_build/dist/bin/
extract/js/src/_build/dist/include/double-conversion/
extract/js/src/_build/dist/include/fdlibm.h
extract/js/src/_build/dist/include/js/
extract/js/src/_build/dist/include/jsapi.h
extract/js/src/_build/dist/include/js-config.h
extract/js/src/_build/dist/include/jsfriendapi.h
extract/js/src/_build/dist/include/jspubtd.h
extract/js/src/_build/dist/include/jstypes.h
extract/js/src/_build/dist/include/malloc_decls.h
extract/js/src/_build/dist/include/mozilla/
extract/js/src/_build/dist/include/mozjemalloc_types.h
extract/js/src/_build/dist/include/mozmemory.h
extract/js/src/_build/dist/include/mozmemory_wrap.h
extract/js/src/_build/.cargo/
extract/js/src/_build/config.statusd/
extract/js/src/_build/faster/
extract/js/src/_build/install_dist_bin.track
extract/js/src/_build/install_dist_include.track
extract/js/src/_build/install_dist_private.track
extract/js/src/_build/install_dist_public.track
extract/js/src/_build/install__tests.track
extract/js/src/_build/js/
extract/js/src/_build/memory/backend.mk
extract/js/src/_build/memory/build/
extract/js/src/_build/memory/Makefile
extract/js/src/_build/memory/mozalloc/
extract/js/src/_build/mfbt/backend.mk
extract/js/src/_build/mfbt/.deps/
extract/js/src/_build/mfbt/Makefile
extract/js/src/_build/modules/fdlibm/backend.mk
extract/js/src/_build/modules/fdlibm/Makefile
extract/js/src/_build/modules/fdlibm/src/backend.mk
extract/js/src/_build/modules/fdlibm/src/.deps/
extract/js/src/_build/modules/fdlibm/src/Makefile
extract/js/src/_build/mozglue/backend.mk
extract/js/src/_build/mozglue/build/backend.mk
extract/js/src/_build/mozglue/build/Makefile
extract/js/src/_build/mozglue/Makefile
extract/js/src/_build/mozglue/misc/backend.mk
extract/js/src/_build/mozglue/misc/.deps/
extract/js/src/_build/mozglue/misc/Makefile
extract/js/src/_build/mozinfo.json
extract/js/src/_build/old-configure.vars
extract/js/src/_build/testing/
extract/js/src/_build/_tests/mozbase/
extract/js/src/_build/third_party/
__XARGS_RM__

xargs rm -r<<__XARGS_RM__
extract/js/src/build
extract/js/src/devtools/rootAnalysis/Makefile.in
extract/js/src/devtools/vprof/manifest.mk
extract/js/src/old-configure
extract/js/src/old-configure.in
extract/js/src/wasm/moz.build
extract/js/src/js-confdefs.h.in
extract/js/src/js-config.h.in
extract/js/src/js-config.mozbuild
extract/js/src/js-cxxflags.mozbuild
extract/js/src/js-standalone.mozbuild
__XARGS_RM__

# ESR 91.0
xargs rm -r<<__XARGS_RM__
extract/js/src/_build
extract/js/src/make-source-package.py
extract/js/src/jsshell.msg
__XARGS_RM__
