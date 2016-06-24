#!/bin/sh

#run this with a mozilla-release directory with the untarred firefox download

rm -rf extract

mkdir extract
mkdir extract/js
mkdir -p extract/intl/icu/source/common/unicode

cp -r mozilla-release/js/src mozilla-release/js/public extract/js/
cp -r mozilla-release/mfbt extract/

# We need these even without ICU
cp mozilla-release/intl/icu/source/common/unicode/platform.h extract/intl/icu/source/common/unicode
cp mozilla-release/intl/icu/source/common/unicode/ptypes.h extract/intl/icu/source/common/unicode
cp mozilla-release/intl/icu/source/common/unicode/uconfig.h extract/intl/icu/source/common/unicode
cp mozilla-release/intl/icu/source/common/unicode/umachine.h extract/intl/icu/source/common/unicode
cp mozilla-release/intl/icu/source/common/unicode/urename.h extract/intl/icu/source/common/unicode
cp mozilla-release/intl/icu/source/common/unicode/utypes.h extract/intl/icu/source/common/unicode
cp mozilla-release/intl/icu/source/common/unicode/uvernum.h extract/intl/icu/source/common/unicode
cp mozilla-release/intl/icu/source/common/unicode/uversion.h extract/intl/icu/source/common/unicode

cd mozilla-release/js/src

# skipping icu and relying on posix nspr emulation all helps.  After that we
# only need js/src, js/public and mfbt.  Well, we also need 8 of the icu
# headers, but only to stub out functions that fail at runtime
PYTHON=python ./configure --without-intl-api --enable-posix-nspr-emulation

# we have to run make to generate a byte code version of the self hosted js and
# a switch table
make

cd ../../..

cp mozilla-release/js/src/js/src/selfhosted.out.h extract/js/src
cp mozilla-release/js/src/js/src/jsautokw.h extract/js/src

# mfbt doesn't change by arch or platform, so keep the same unified cpp
mkdir extract/js/src/mfbt
cp mozilla-release/js/src/mfbt/Unified_cpp_mfbt0.cpp extract/js/src/mfbt

sed 's/#include ".*\/mfbt\//#include "/' < extract/js/src/mfbt/Unified_cpp_mfbt0.cpp > t1
sed 's/#error ".*\/mfbt\//#error "/' < t1 > extract/js/src/mfbt/Unified_cpp_mfbt0.cpp
rm t1

# stuff we can toss
rm -rf \
    extract/js/src/all-tests.json \
    extract/js/src/backend.mk \
    extract/js/src/backend.RecursiveMakeBackend \
    extract/js/src/backend.RecursiveMakeBackend.pp \
    extract/js/src/_build_manifests \
    extract/js/src/config \
    extract/js/src/config.cache \
    extract/js/src/config.log \
    extract/js/src/config.status \
    extract/js/src/configure \
    extract/js/src/configure.in \
    extract/js/src/ctypes \
    extract/js/src/doc \
    extract/js/src/editline \
    extract/js/src/gdb \
    extract/js/src/ipc \
    extract/js/src/jit-test \
    extract/js/src/jsapi-tests \
    extract/js/src/Makefile \
    extract/js/src/Makefile.in \
    extract/js/src/make-source-package.sh \
    extract/js/src/octane \
    extract/js/src/python \
    extract/js/src/README.html \
    extract/js/src/root-deps.mk \
    extract/js/src/root.mk \
    extract/js/src/shell \
    extract/js/src/skip_subconfigures \
    extract/js/src/subconfigures \
    extract/js/src/tests \
    extract/js/src/_virtualenv

# stuff we have to replace
rm -rf \
    extract/js/src/vm/PosixNSPR.cpp \
    extract/js/src/vm/PosixNSPR.h \

# stuff we don't want to deal with due to licensing
rm -rf \
    extract/mfbt/decimal \
    extract/mfbt/tests \
    extract/js/src/vm/make_unicode.py \
    extract/js/src/vtune

# this is all of the EXPORTS files from the moz.build's
mkdir include
cp extract/js/src/js.msg include
cp extract/js/src/jsalloc.h include
cp extract/js/src/jsapi.h include
cp extract/js/src/jsbytecode.h include
cp extract/js/src/jsclist.h include
cp extract/js/src/jscpucfg.h include
cp extract/js/src/jsfriendapi.h include
cp extract/js/src/jsprf.h include
cp extract/js/src/jsprototypes.h include
cp extract/js/src/jspubtd.h include
cp extract/js/src/jstypes.h include
cp extract/js/src/jsversion.h include
cp extract/js/src/jswrapper.h include
cp extract/js/src/perf/jsperf.h include

# this is all of the EXPORTS.js files from the moz.build's
mkdir include/js
cp extract/js/public/CallArgs.h include/js
cp extract/js/public/CallNonGenericMethod.h include/js
cp extract/js/public/CharacterEncoding.h include/js
cp extract/js/public/Class.h include/js
cp extract/js/public/Conversions.h include/js
cp extract/js/public/Date.h include/js
cp extract/js/public/Debug.h include/js
cp extract/js/public/GCAPI.h include/js
cp extract/js/public/GCHashTable.h include/js
cp extract/js/public/HashTable.h include/js
cp extract/js/public/HeapAPI.h include/js
cp extract/js/public/Id.h include/js
cp extract/js/public/Initialization.h include/js
cp extract/js/public/LegacyIntTypes.h include/js
cp extract/js/public/MemoryMetrics.h include/js
cp extract/js/public/Principals.h include/js
cp extract/js/public/ProfilingFrameIterator.h include/js
cp extract/js/public/ProfilingStack.h include/js
cp extract/js/public/Proxy.h include/js
cp extract/js/public/RequiredDefines.h include/js
cp extract/js/public/RootingAPI.h include/js
cp extract/js/public/SliceBudget.h include/js
cp extract/js/public/StructuredClone.h include/js
cp extract/js/public/TraceKind.h include/js
cp extract/js/public/TraceableVector.h include/js
cp extract/js/public/TracingAPI.h include/js
cp extract/js/public/TrackedOptimizationInfo.h include/js
cp extract/js/public/TypeDecls.h include/js
cp extract/js/public/UbiNode.h include/js
cp extract/js/public/UbiNodeBreadthFirst.h include/js
cp extract/js/public/UbiNodeCensus.h include/js
cp extract/js/public/Utility.h include/js
cp extract/js/public/Value.h include/js
cp extract/js/public/Vector.h include/js
cp extract/js/public/WeakMapPtr.h include/js

# this is all of the EXPORTS.mozilla files from the moz.build's
mkdir include/mozilla
cp extract/mfbt/Alignment.h include/mozilla
cp extract/mfbt/AllocPolicy.h include/mozilla
cp extract/mfbt/AlreadyAddRefed.h include/mozilla
cp extract/mfbt/Array.h include/mozilla
cp extract/mfbt/ArrayUtils.h include/mozilla
cp extract/mfbt/Assertions.h include/mozilla
cp extract/mfbt/Atomics.h include/mozilla
cp extract/mfbt/Attributes.h include/mozilla
cp extract/mfbt/BinarySearch.h include/mozilla
cp extract/mfbt/BloomFilter.h include/mozilla
cp extract/mfbt/Casting.h include/mozilla
cp extract/mfbt/ChaosMode.h include/mozilla
cp extract/mfbt/Char16.h include/mozilla
cp extract/mfbt/CheckedInt.h include/mozilla
cp extract/mfbt/Compiler.h include/mozilla
cp extract/mfbt/Compression.h include/mozilla
cp extract/mfbt/DebugOnly.h include/mozilla
cp extract/mfbt/double-conversion/double-conversion.h include/mozilla
cp extract/mfbt/double-conversion/utils.h include/mozilla
cp extract/mfbt/Endian.h include/mozilla
cp extract/mfbt/EnumeratedArray.h include/mozilla
cp extract/mfbt/EnumeratedRange.h include/mozilla
cp extract/mfbt/EnumSet.h include/mozilla
cp extract/mfbt/FastBernoulliTrial.h include/mozilla
cp extract/mfbt/FloatingPoint.h include/mozilla
cp extract/mfbt/GuardObjects.h include/mozilla
cp extract/mfbt/HashFunctions.h include/mozilla
cp extract/mfbt/IntegerPrintfMacros.h include/mozilla
cp extract/mfbt/IntegerRange.h include/mozilla
cp extract/mfbt/IntegerTypeTraits.h include/mozilla
cp extract/mfbt/JSONWriter.h include/mozilla
cp extract/mfbt/Likely.h include/mozilla
cp extract/mfbt/LinkedList.h include/mozilla
cp extract/mfbt/LinuxSignal.h include/mozilla
cp extract/mfbt/WindowsVersion.h include/mozilla
cp extract/mfbt/MacroArgs.h include/mozilla
cp extract/mfbt/MacroForEach.h include/mozilla
cp extract/mfbt/MathAlgorithms.h include/mozilla
cp extract/mfbt/Maybe.h include/mozilla
cp extract/mfbt/MaybeOneOf.h include/mozilla
cp extract/mfbt/MemoryChecking.h include/mozilla
cp extract/mfbt/MemoryReporting.h include/mozilla
cp extract/mfbt/Move.h include/mozilla
cp extract/mfbt/NullPtr.h include/mozilla
cp extract/mfbt/NumericLimits.h include/mozilla
cp extract/mfbt/Pair.h include/mozilla
cp extract/mfbt/PodOperations.h include/mozilla
cp extract/mfbt/Poison.h include/mozilla
cp extract/mfbt/Range.h include/mozilla
cp extract/mfbt/RangedPtr.h include/mozilla
cp extract/mfbt/RefCountType.h include/mozilla
cp extract/mfbt/ReentrancyGuard.h include/mozilla
cp extract/mfbt/RefPtr.h include/mozilla
cp extract/mfbt/ReverseIterator.h include/mozilla
cp extract/mfbt/RollingMean.h include/mozilla
cp extract/mfbt/Scoped.h include/mozilla
cp extract/mfbt/ScopeExit.h include/mozilla
cp extract/mfbt/SegmentedVector.h include/mozilla
cp extract/mfbt/SHA1.h include/mozilla
cp extract/mfbt/SizePrintfMacros.h include/mozilla
cp extract/mfbt/Snprintf.h include/mozilla
cp extract/mfbt/SplayTree.h include/mozilla
cp extract/mfbt/TaggedAnonymousMemory.h include/mozilla
cp extract/mfbt/TemplateLib.h include/mozilla
cp extract/mfbt/ThreadLocal.h include/mozilla
cp extract/mfbt/ToString.h include/mozilla
cp extract/mfbt/TypedEnumBits.h include/mozilla
cp extract/mfbt/Types.h include/mozilla
cp extract/mfbt/TypeTraits.h include/mozilla
cp extract/mfbt/UniquePtr.h include/mozilla
cp extract/mfbt/Variant.h include/mozilla
cp extract/mfbt/Vector.h include/mozilla
cp extract/mfbt/WeakPtr.h include/mozilla
cp extract/mfbt/unused.h include/mozilla
cp extract/mfbt/XorShift128PlusRNG.h include/mozilla

# Apply a local patch
git apply \
    mongodb_patches/SERVER-23358.patch \
    mongodb_patches/SERVER-24400.patch \
    mongodb_patches/SERVER-22927-x86_64.patch
