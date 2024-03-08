/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include <absl/hash/hash.h>

// IWYU pragma: no_include "cxxabi.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <ostream>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/data_range.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/base/static_assert.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/tcmalloc_parameters_gen.h"

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

#include <MurmurHash3.h>

#ifdef MONGO_CONFIG_TCMALLOC_GPERF
#include <gperftools/malloc_hook.h>
#endif

#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
#include <absl/debugging/symbolize.h>
#include <tcmalloc/malloc_extension.h>
#endif

#if defined(_POSIX_VERSION) && defined(MONGO_CONFIG_HAVE_EXECINFO_BACKTRACE)
#include <dlfcn.h>
#include <execinfo.h>
#endif


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#if defined(_POSIX_VERSION) && defined(MONGO_CONFIG_HAVE_EXECINFO_BACKTRACE) && \
    (defined(MONGO_CONFIG_TCMALLOC_GPERF) || defined(MONGO_CONFIG_TCMALLOC_GOOGLE))


namespace mongo {
namespace {

/** Simple wrapper for the demangler, particularly its buffer space. */
class Demangler {
public:
    Demangler() = default;
    Demangler(const Demangler&) = delete;
    ~Demangler() {
        free(_buf);
    }

    char* operator()(const char* sym) {
        char* dm = abi::__cxa_demangle(sym, _buf, &_bufSize, &_status);
        if (dm)
            _buf = dm;
        return dm;
    }

private:
    size_t _bufSize = 0;
    char* _buf = nullptr;
    int _status = 0;
};


//
// Sampling heap profiler
//
// Intercepts allocate and free calls to track approximate number of live allocated bytes
// associated with each allocating stack trace at each point in time.
//
// Hooks into tcmalloc via the MallocHook interface, but has no dependency
// on any allocator internals; could be used with any allocator via similar
// hooks, or via shims.
//
// Adds no space overhead to each allocated object - allocated objects
// and associated stack traces are recorded in separate pre-allocated
// fixed-size hash tables. Size of the separate hash tables is configurable
// but something on the order of tens of MB should suffice for most purposes.
//
// Performance overhead is small because it only samples a fraction of the allocations.
//
// Samples allocate calls every so many bytes allocated.
//   * a stack trace is obtained, and entered in a stack hash table if it's a new stack trace
//   * the number of active bytes charged to that stack trace is increased
//   * the allocated object, stack trace, and number of bytes is recorded in an object hash table
// For each free call if the freed object is in the object hash table.
//   * the number of active bytes charged to the allocating stack trace is decreased
//   * the object is removed from the object hash table
//
// Enable at startup time (only) with
//     mongod --setParameter heapProfilingEnabled=true
//
// If enabled, adds a heapProfile section to serverStatus as follows:
//
// heapProfile: {
//     stats: {
//         //  internal stats related to heap profiling process (collisions, number of stacks, etc.)
//     }
//     stacks: {
//         stack_n_: {             // one for each stack _n_
//             activeBytes: ...,   // number of active bytes allocated by this stack
//             stack: [            // the stack itself
//                 "frame0",
//                 "frame1",
//                 ...
//            ]
//        }
//    }
//
// Each new stack encountered is also logged to mongod log with a message like
//     .... stack_n_: {0: "frame0", 1: "frame1", ...}
//
// Can be used in one of two ways:
//
// Via FTDC - strings are not captured by FTDC, so the information
// recorded in FTDC for each sample is essentially of the form
// {stack_n_: activeBytes} for each stack _n_. The timeseries tool
// will present one graph per stack, identified by the label stack_n_,
// showing active bytes that were allocated by that stack at each
// point in time. The mappings from stack_n_ to the actual stack can
// be found in mongod log.
//
// Via serverStatus - the serverStatus section described above
// contains complete information, including the stack trace.  It can
// be obtained and examined manually, and can be further processed by
// tools.
//
// We will need about 1 active ObjInfo for every sampleIntervalBytes live bytes,
// so max active memory we can handle is sampleIntervalBytes * kMaxObjInfos.
// With the current defaults of
//     kMaxObjInfos = 1024 * 1024
//     sampleIntervalBytes = 256 * 1024
// the following information is computed and logged on startup (see HeapProfiler()):
//     maxActiveMemory 262144 MB
//     objTableSize 72 MB
//     stackTableSize 16.6321MB
// So the defaults allow handling very large memories at a reasonable sampling interval
// and acceptable size overhead for the hash tables.
//

// TODO SERVER-44010: Consider replacing this custom implementation with a generic one.
//
// Simple hash table maps Key->Value.
// All storage is pre-allocated at creation.
// Access functions take a hash specifying a bucket as the first parameter to avoid re-computing
// hash unnecessarily; caller must ensure that hash is correctly computed from the appropriate Key.
// Key must implement operator== to support find().
// Key and Value must both support assignment to allow copying key and value into table on insert.
//
// Concurrency rules:
//     Reads (find(), isBucketEmpty(), forEach()) MAY be called concurrently with each other.
//     Writes (insert(), remove()) may NOT be called concurrently with each other.
//     Concurrency of reads and writes is as follows:
//         find() may NOT be called concurrently with any write.
//         isBucketEmpty() MAY be called concurrently with any write.
//         forEach()
//             MAY be called concurrently with insert() but NOT remove()
//             does not provide snapshot semantics wrt set of entries traversed
//             caller must ensure safety wrt concurrent modification of Value of existing entry
//

using Hash = size_t;

template <class Key, class Value>
class HashTable {
public:
    HashTable(size_t maxEntries, int loadFactor)
        : maxEntries(maxEntries),
          numEntries(0),
          numBuckets(maxEntries * loadFactor),
          buckets(new std::atomic<Entry*>[numBuckets]()),  // NOLINT
          entries(new Entry[maxEntries]()),
          nextEntry(0),
          freeEntry(nullptr) {}

    // Allocate a new entry in the specified hash bucket.
    // Stores a copy of the specified Key and Value.
    // Returns a pointer to the newly allocated Value, or nullptr if out of space.
    Value* insert(Hash hash, const Key& key, const Value& value) {
        hash %= numBuckets;
        Entry* entry = nullptr;
        if (freeEntry) {
            entry = freeEntry;
            freeEntry = freeEntry->next;
        } else if (nextEntry < maxEntries) {
            entry = &entries[nextEntry++];
        }
        if (!entry)
            return nullptr;
        entry->next = buckets[hash].load();
        buckets[hash] = entry;
        entry->key = key;
        entry->value = value;
        entry->valid = true;  // signal that the entry is well-formed and may be traversed
        ++numEntries;
        return &entry->value;
    }

    // Find the entry containing Key in the specified hash bucket.
    // Returns a pointer to the corresponding Value object, or nullptr if not found.
    Value* find(Hash hash, const Key& key) {
        hash %= numBuckets;
        for (Entry* entry = buckets[hash]; entry; entry = entry->next)
            if (entry->key == key)
                return &entry->value;
        return nullptr;
    }

    // Remove an entry specified by key.
    void remove(Hash hash, const Key& key) {
        hash %= numBuckets;
        for (auto nextp = &buckets[hash]; *nextp; nextp = &((*nextp).load()->next)) {
            Entry* entry = *nextp;
            if (entry->key == key) {
                *nextp = entry->next.load();
                entry->valid = false;  // first signal entry is invalid as it may get reused
                entry->next = freeEntry;
                freeEntry = entry;
                --numEntries;
                break;
            }
        }
    }

    // Traverse the array of pre-allocated entries, calling f(key, value) on each valid entry.
    // This may be called concurrently with insert() but not remove()
    //     atomic entry.valid ensures that it will see only well-formed entries
    //     nextEntry is atomic to guard against torn reads as nextEntry is updated
    // Note however it is not guaranteed to provide snapshot semantics wrt the set of entries,
    // and caller must ensure safety wrt concurrent updates to the Value of an entry
    template <typename F>
    void forEach(const F& f) {
        for (size_t i = 0; i < nextEntry; i++) {
            Entry& e = entries[i];
            if (e.valid)  // only traverse well-formed entries
                f(e.key, e.value);
        }
    }

    // Determines whether the specified hash bucket is empty. May be called concurrently with
    // insert() and remove(). Concurrent visibility on other threads is guaranteed because
    // buckets[hash] is atomic.
    bool isEmptyBucket(Hash hash) const {
        return !buckets[hash % numBuckets];
    }

    // Number of entries.
    size_t size() const {
        return numEntries;
    }

    // Highwater mark of number of entries used, for reporting stats.
    size_t maxSizeSeen() const {
        return nextEntry;
    }

    // Returns total allocated size of the hash table, for reporting stats.
    size_t memorySizeBytes() const {
        return numBuckets * sizeof(buckets[0]) + maxEntries * sizeof(entries[0]);
    }

private:
    struct Entry {
        Key key{};
        Value value{};
        std::atomic<Entry*> next{nullptr};  // NOLINT
        std::atomic<bool> valid{false};     // NOLINT
    };

    const size_t maxEntries;        // we allocate storage for this many entries on creation
    std::atomic_size_t numEntries;  // number of entries currently in use  NOLINT
    size_t numBuckets;              // number of buckets, computed as numEntries * loadFactor

    // pre-allocate buckets and entries
    std::unique_ptr<std::atomic<Entry*>[]> buckets;  // NOLINT
    std::unique_ptr<Entry[]> entries;

    std::atomic_size_t nextEntry;  // first entry that's never been used  NOLINT
    Entry* freeEntry;              // linked list of entries returned to us by removeEntry
};

namespace heap_profiler_detail_gperf_tcmalloc {
class HeapProfiler {
public:
    static inline HeapProfiler* heapProfiler;

    HeapProfiler() {
        // Set sample interval from the parameter.
        sampleIntervalBytes = HeapProfilingSampleIntervalBytes;

        // This is our only allocator dependency - ifdef and change as
        // appropriate for other allocators, using hooks or shims.
        // For tcmalloc we skip two frames that are internal to the allocator
        // so that the top frame is the public tc_* function.
        skipStartFrames = 2;
        skipEndFrames = 0;
#ifdef MONGO_CONFIG_TCMALLOC_GPERF
        MallocHook::AddNewHook(+[](const void* p, size_t sz) { heapProfiler->_alloc(p, sz); });
        MallocHook::AddDeleteHook(+[](const void* p) { heapProfiler->_free(p); });
#endif
    }

    static void generateServerStatusSection(BSONObjBuilder& builder) {
        if (heapProfiler)
            heapProfiler->_generateServerStatusSection(builder);
    }

    static void start() {
        heapProfiler = new HeapProfiler();
    }

private:
    using FrameInfo = void*;  // per-frame information is just the IP

    static const int kMaxStackInfos = 20000;         // max number of unique call sites we handle
    static const int kStackHashTableLoadFactor = 2;  // keep loading <50%
    static const size_t kMaxFramesPerStack = 100;    // max depth of stack

    static const int kMaxObjInfos = 1024 * 1024;   // maximum tracked allocations
    static const int kObjHashTableLoadFactor = 4;  // keep hash table loading <25%

    static const int kMaxImportantSamples = 4 * 3600;  // reset every 4 hours at 1Hz

    // stack HashTable Key
    struct Stack {
        Stack() = default;

        friend bool operator==(const Stack& a, const Stack& b) {
            return a.numFrames == b.numFrames &&
                std::equal(a.frames.begin(), a.frames.begin() + a.numFrames, b.frames.begin());
        }

        Hash hash() {
            MONGO_STATIC_ASSERT_MSG(sizeof(frames) == sizeof(FrameInfo) * kMaxFramesPerStack,
                                    "frames array is not dense");
            ConstDataRange dataRange{reinterpret_cast<const char*>(frames.data()),
                                     numFrames * sizeof(FrameInfo)};
            return absl::HashOf(dataRange);
        }

        size_t numFrames = 0;
        std::array<FrameInfo, kMaxFramesPerStack> frames;
    };

    // Stack HashTable Value.
    struct StackInfo {
        StackInfo() = default;
        explicit StackInfo(int stackNum) : stackNum(stackNum) {}

        int stackNum = 0;        // used for stack short name
        size_t activeBytes = 0;  // number of live allocated bytes charged to this stack
        bool logged = false;     // true when stack has been logged once.
    };

    struct ByPointeeStackNum {
        bool operator()(const StackInfo* a, const StackInfo* b) const {
            return a->stackNum < b->stackNum;
        }
    };

    struct Obj {
        Obj() = default;
        explicit Obj(const void* objPtr) : objPtr(objPtr) {}

        friend bool operator==(const Obj& a, const Obj& b) {
            return a.objPtr == b.objPtr;
        }

        Hash hash() {
            return absl::HashOf(objPtr);
        }

        const void* objPtr = nullptr;
    };

    struct ObjInfo {
        ObjInfo() = default;
        ObjInfo(size_t accountedLen, StackInfo* stackInfo)
            : accountedLen(accountedLen), stackInfo(stackInfo) {}

        size_t accountedLen = 0;
        StackInfo* stackInfo = nullptr;
    };

    // If we encounter an error that doesn't allow us to proceed, for
    // example out of space for new hash table entries, we internally
    // disable profiling and then log an error message.
    void disable(const char* msg) {
        sampleIntervalBytes = 0;
        LOGV2(23157, "{msg}", "msg"_attr = msg);
    }

    //
    // Record an allocation.
    //
    void _alloc(const void* objPtr, size_t objLen) {
        // still profiling?
        if (sampleIntervalBytes == 0)
            return;

        // Sample every sampleIntervalBytes bytes of allocation.
        // We charge each sampled stack with the amount of memory allocated since the last sample
        // this could grossly overcharge any given stack sample, but on average over a large
        // number of samples will be correct.
        size_t lastSample = bytesAllocated.fetch_add(objLen);
        size_t currentSample = lastSample + objLen;
        size_t accountedLen = sampleIntervalBytes *
            (currentSample / sampleIntervalBytes - lastSample / sampleIntervalBytes);
        if (accountedLen == 0)
            return;

        // Get backtrace.
        Stack tempStack;
        tempStack.numFrames = rawBacktrace(tempStack.frames.data(), kMaxFramesPerStack);

        // Compute backtrace hash.
        Hash stackHash = tempStack.hash();

        // Now acquire lock.
        stdx::lock_guard<stdx::mutex> lk(hashtable_mutex);

        // Look up stack in stackHashTable.
        StackInfo* stackInfo = stackHashTable.find(stackHash, tempStack);

        // If new stack, store in stackHashTable.
        if (!stackInfo) {
            StackInfo newStackInfo(stackHashTable.size() /*stackNum*/);
            stackInfo = stackHashTable.insert(stackHash, tempStack, newStackInfo);
            if (!stackInfo) {
                disable("too many stacks; disabling heap profiling");
                return;
            }
        }

        // Count the bytes.
        totalActiveBytes += accountedLen;
        stackInfo->activeBytes += accountedLen;

        // Enter obj in objHashTable.
        Obj obj(objPtr);
        ObjInfo objInfo(accountedLen, stackInfo);
        if (!objHashTable.insert(obj.hash(), obj, objInfo)) {
            disable("too many live objects; disabling heap profiling");
            return;
        }
    }

    //
    // Record a freed object.
    //
    void _free(const void* objPtr) {
        // still profiling?
        if (sampleIntervalBytes == 0)
            return;

        // Compute hash, quick return before locking if bucket is empty (common case).
        // This is crucial for performance because we need to check the hash table on every _free.
        // Visibility of the bucket entry if the _alloc was done on a different thread is
        // guaranteed because isEmptyBucket consults an atomic pointer.
        Obj obj(objPtr);
        Hash objHash = obj.hash();
        if (objHashTable.isEmptyBucket(objHash))
            return;

        // Now acquire lock.
        stdx::lock_guard<stdx::mutex> lk(hashtable_mutex);

        // Remove the object from the hash bucket if present.
        ObjInfo* objInfo = objHashTable.find(objHash, obj);
        if (objInfo) {
            totalActiveBytes -= objInfo->accountedLen;
            objInfo->stackInfo->activeBytes -= objInfo->accountedLen;
            objHashTable.remove(objHash, obj);
        }
    }

    //
    // Generate bson representation of stack.
    //
    void generateStack(StackTraceAddressMetadataGenerator& metaGen,
                       Demangler& demangler,
                       Stack& stack,
                       const StackInfo& stackInfo) {
        BSONArrayBuilder builder;

        std::string frameString;

        size_t jb = std::min(stack.numFrames, skipStartFrames);
        size_t je = stack.numFrames - std::min(stack.numFrames - jb, skipEndFrames);
        for (size_t j = jb; j != je; ++j) {
            frameString.clear();
            void* addr = stack.frames[j];
            const auto& meta = metaGen.load(addr);
            if (meta.symbol()) {
                if (StringData name = meta.symbol().name(); !name.empty()) {
                    // Upgrade frameString to symbol name.
                    frameString.assign(name.begin(), name.end());
                    if (char* dm = demangler(frameString.c_str())) {
                        // Further upgrade frameString to demangled name.
                        // We strip function parameters as they are very verbose and not useful.
                        frameString = dm;
                        if (auto paren = frameString.find('('); paren != std::string::npos)
                            frameString.erase(paren);
                    }
                }
            }
            if (frameString.empty()) {
                // Fall back to frameString as stringified `void*`.
                std::ostringstream s;
                s << addr;
                frameString = s.str();
            }
            builder.append(frameString);
        }
        LOGV2(23158,
              "heapProfile stack",
              "stackNum"_attr = stackInfo.stackNum,
              "stackObj"_attr = builder.done());
    }

    void _generateServerStatusSection(BSONObjBuilder& builder) {
        // compute and log some informational stats first time through
        if (logGeneralStats) {
            const size_t maxActiveMemory = sampleIntervalBytes * kMaxObjInfos;
            const size_t objTableSize = objHashTable.memorySizeBytes();
            const size_t stackTableSize = stackHashTable.memorySizeBytes();
            const double MB = 1024 * 1024;
            LOGV2(23159,
                  "Generating heap profiler serverStatus",
                  "heapProfilingSampleIntervalBytes"_attr = HeapProfilingSampleIntervalBytes,
                  "maxActiveMemoryMB"_attr = maxActiveMemory / MB,
                  "objTableSize_MB"_attr = objTableSize / MB,
                  "stackTableSizeMB"_attr = stackTableSize / MB);
            // print a stack trace to log somap for post-facto symbolization
            LOGV2(23160, "Following stack trace is for heap profiler informational purposes");
            printStackTrace();
            logGeneralStats = false;
        }

        // Stats subsection.
        BSONObjBuilder statsBuilder(builder.subobjStart("stats"));
        statsBuilder.appendNumber("totalActiveBytes", static_cast<long long>(totalActiveBytes));
        statsBuilder.appendNumber("bytesAllocated", static_cast<long long>(bytesAllocated));
        statsBuilder.appendNumber("numStacks", static_cast<long long>(stackHashTable.size()));
        statsBuilder.appendNumber("currentObjEntries", static_cast<long long>(objHashTable.size()));
        statsBuilder.appendNumber("maxObjEntriesUsed",
                                  static_cast<long long>(objHashTable.maxSizeSeen()));
        statsBuilder.doneFast();

        // Guard against races updating the StackInfo bson representation.
        stdx::lock_guard<stdx::mutex> lk(stackinfo_mutex);

        // Traverse stackHashTable accumulating potential stacks to emit.
        // We do this traversal without locking hashtable_mutex because we need to use the heap.
        // forEach guarantees this is safe wrt to insert(), and we never call remove().
        // We use stackinfo_mutex to ensure safety wrt concurrent updates to the StackInfo objects.
        // We can get skew between entries, which is ok.
        struct HeapEntry {
            const StackInfo* info;
            size_t activeBytes;  // snapshot `info->activeBytes` because it changes during sort.
        };
        std::vector<HeapEntry> heap;
        size_t snapshotTotal = 0;

        StackTraceAddressMetadataGenerator metaGen;
        Demangler demangler;
        stackHashTable.forEach([&](Stack& stack, StackInfo& stackInfo) {
            if (auto bytes = stackInfo.activeBytes) {
                if (!std::exchange(stackInfo.logged, true)) {
                    generateStack(metaGen, demangler, stack, stackInfo);
                }
                heap.push_back({&stackInfo, bytes});
                snapshotTotal += bytes;
            }
        });
        auto heapEnd = heap.end();

        // Sort the stacks and find enough stacks to account for at least 99% of the active bytes
        // deem any stack that has ever met this criterion as "important".
        // Using heap structure to avoid comparing elements that won't make the cut anyway.
        auto heapCompare = [](auto&& a, auto&& b) {
            return a.activeBytes > b.activeBytes;
        };
        std::make_heap(heap.begin(), heapEnd, heapCompare);

        size_t threshold = totalActiveBytes * 0.99;
        size_t cumulative = 0;
        size_t previous = 0;
        while (heapEnd != heap.begin()) {
            const auto& front = heap.front();
            if (cumulative > threshold && front.activeBytes > previous)
                break;
            importantStacks.insert(front.info);
            previous = front.activeBytes;
            cumulative += front.activeBytes;
            std::pop_heap(heap.begin(), heapEnd--, heapCompare);
        }

        // Build the stacks subsection by emitting the "important" stacks.
        BSONObjBuilder stacksBuilder(builder.subobjStart("stacks"));
        for (auto it = importantStacks.begin(); it != importantStacks.end(); ++it) {
            const StackInfo* stackInfo = *it;
            std::ostringstream shortName;
            shortName << "stack" << stackInfo->stackNum;
            BSONObjBuilder stackBuilder(stacksBuilder.subobjStart(shortName.str()));
            stackBuilder.appendNumber("activeBytes",
                                      static_cast<long long>(stackInfo->activeBytes));
        }
        stacksBuilder.doneFast();

        // importantStacks grows monotonically, so it can accumulate unneeded stacks,
        // so we clear it periodically.
        if (++numImportantSamples >= kMaxImportantSamples) {
            LOGV2(23161, "Clearing importantStacks");
            importantStacks.clear();
            numImportantSamples = 0;
        }
    }

    // 0: sampling internally disabled
    // 1: sample every allocation - byte accurate but slow and big
    // >1: sample ever sampleIntervalBytes bytes allocated - less accurate but fast and small
    std::atomic_size_t sampleIntervalBytes;  // NOLINT

    // guards updates to both object and stack hash tables
    stdx::mutex hashtable_mutex;  // NOLINT
    // guards against races updating the StackInfo bson representation
    stdx::mutex stackinfo_mutex;  // NOLINT

    // cumulative bytes allocated - determines when samples are taken
    std::atomic_size_t bytesAllocated{0};  // NOLINT

    // estimated currently active bytes - sum of activeBytes for all stacks
    size_t totalActiveBytes = 0;

    // The stack HashTable itself.
    HashTable<Stack, StackInfo> stackHashTable{kMaxStackInfos, kStackHashTableLoadFactor};

    // frames to skip at top and bottom of backtrace when reporting stacks
    size_t skipStartFrames = 0;
    size_t skipEndFrames = 0;

    // The obj HashTable itself.
    HashTable<Obj, ObjInfo> objHashTable{kMaxObjInfos, kObjHashTableLoadFactor};

    bool logGeneralStats = true;  // first time only

    // In order to reduce load on ftdc we track the stacks we deem important enough to emit
    // once a stack is deemed "important" it remains important from that point on.
    // "Important" is a sticky quality to improve the stability of the set of stacks we emit,
    // and we always emit them in stackNum order, greatly improving ftdc compression efficiency.
    std::set<const StackInfo*, ByPointeeStackNum> importantStacks;

    int numImportantSamples = 0;  // samples currently included in importantStacks
};
}  // namespace heap_profiler_detail_gperf_tcmalloc

namespace heap_profiler_detail_tcmalloc {
#ifdef MONGO_CONFIG_TCMALLOC_GOOGLE
class HeapProfiler {
public:
    static const int kMaxImportantSamples = 4 * 3600;  // reset every 4 hours at 1Hz
    static inline HeapProfiler* heapProfiler;

    HeapProfiler() {
        sampleIntervalBytes = HeapProfilingSampleIntervalBytes;
        tcmalloc::MallocExtension::SetProfileSamplingRate(sampleIntervalBytes);
        auto profileToken = tcmalloc::MallocExtension::StartAllocationProfiling();
        profileTokens.push_back(std::move(profileToken));
    }

    static void generateServerStatusSection(BSONObjBuilder& builder) {
        if (heapProfiler)
            heapProfiler->_generateServerStatusSection(builder);
    }

    static void start() {
        heapProfiler = new HeapProfiler();
    }

private:
    struct StackInfo {
        StackInfo(const tcmalloc::Profile::Sample& stackSample, int id) {
            stackNum = id;
            numFrames = stackSample.depth;
            // Generate a bson representation of our new stack.
            BSONArrayBuilder builder;
            std::string frameString(256, '\0');
            for (int i = 0; i < stackSample.depth; ++i) {
                char buf[256];
                if (!absl::Symbolize(stackSample.stack[i], buf, sizeof(buf))) {
                    frameString = fmt::format("{}", stackSample.stack[i]);
                } else {
                    frameString.assign(buf);
                }
                builder.append(frameString);
            }
            LOGV2(8592501,
                  "heapProfile stack",
                  "stackNum"_attr = stackNum,
                  "stackObj"_attr = builder.obj());
        }

        int stackNum = 0;  // used for stack short name
        BSONObj stackObj;  // symbolized representation
        int numFrames = 0;
        uint64_t activeBytes = 0;
    };

    struct ByStackNum {
        bool operator()(StackInfo* a, StackInfo* b) const {
            return a->stackNum < b->stackNum;
        }
    };

    uint32_t StackHash(const tcmalloc::Profile::Sample& stackSample) {
        uint32_t hash;
        MurmurHash3_x86_32(stackSample.stack, stackSample.depth * sizeof(void*), 0, &hash);
        return hash;
    }

    void _generateServerStatusSection(BSONObjBuilder& builder) {
        // Compute and log some informational stats first time through
        if (logGeneralStats) {
            LOGV2(8592504,
                  "Generating heap profiler serverStatus",
                  "heapProfilingSampleIntervalBytes"_attr = HeapProfilingSampleIntervalBytes);
            LOGV2(8592503, "Following stack trace is for heap profiler informational purposes");
            printStackTrace();
            logGeneralStats = false;
        }

        // Get a live snapshot profile of the current heap usage
        int64_t totalActiveBytes = 0;
        std::vector<StackInfo*> stackInfos;
        std::set<StackInfo*, ByStackNum> activeStacks;
        tcmalloc::MallocExtension::SnapshotCurrent(tcmalloc::ProfileType::kHeap)
            .Iterate([&](const auto& sample) {
                totalActiveBytes += sample.sum;
                // Compute backtrace hash of sample stack
                uint32_t stackHash = StackHash(sample);
                StackInfo* stackInfo = stackInfoMap[stackHash];
                // If this is a new stack, store in our stack map
                if (!stackInfo) {
                    stackInfo = new StackInfo(sample, stackInfoMap.size());
                    stackInfoMap[stackHash] = stackInfo;
                }
                auto activeStackSearch = activeStacks.find(stackInfo);
                if (activeStackSearch != activeStacks.end()) {
                    stackInfo->activeBytes += sample.sum;
                } else {
                    activeStacks.insert(stackInfo);
                    stackInfos.push_back(stackInfo);
                    stackInfo->activeBytes = sample.sum;
                }
            });

        // Get the series of allocation samples to this point
        auto currentToken = std::move(profileTokens.back());
        profileTokens.pop_back();
        auto allocProfile = std::move(currentToken).Stop();
        // Start a new allocation profile session for the next invocation
        auto newToken = tcmalloc::MallocExtension::StartAllocationProfiling();
        profileTokens.push_back(std::move(newToken));

        // Sum all the allocations performed (of what we sampled)
        int64_t allocatedBytes = 0;
        allocProfile.Iterate(
            [&](const tcmalloc::Profile::Sample& sample) { allocatedBytes += sample.sum; });
        sampleBytesAllocated += allocatedBytes;

        BSONObjBuilder(builder.subobjStart("stats"))
            .appendNumber("totalActiveBytes", static_cast<long long>(totalActiveBytes))
            .appendNumber("bytesAllocated", static_cast<long long>(sampleBytesAllocated))
            .appendNumber("numStacks", static_cast<long long>(stackInfoMap.size()));

        // Sort the stacks and find enough stacks to account for at least 99% of the active bytes
        // deem any stack that has ever met this criterion as "important".
        std::stable_sort(stackInfos.begin(), stackInfos.end(), [](StackInfo* a, StackInfo* b) {
            return a->activeBytes > b->activeBytes;
        });
        size_t threshold = totalActiveBytes * 0.99;
        size_t cumulative = 0;
        for (auto&& stackInfo : stackInfos) {
            importantStacks.insert(stackInfo);
            cumulative += stackInfo->activeBytes;
            if (cumulative > threshold)
                break;
        }

        // Build the stacks subsection by emitting a sample of stacks that were live at a peak of
        // total heap usage.
        {
            BSONObjBuilder stacks(builder.subobjStart("stacks"));
            for (auto&& stackInfo : importantStacks)
                BSONObjBuilder{stacks.subobjStart(fmt::format("stack{}", stackInfo->stackNum))}
                    .appendNumber("activeBytes", static_cast<long long>(stackInfo->activeBytes));
        }

        // importantStacks grows monotonically, so it can accumulate unneeded stacks,
        // so we clear it periodically.
        if (++numImportantSamples >= kMaxImportantSamples) {
            LOGV2(8592502, "Clearing importantStacks");
            importantStacks.clear();
            numImportantSamples = 0;
        }
    }

    std::vector<tcmalloc::MallocExtension::AllocationProfilingToken> profileTokens;
    std::atomic_size_t sampleIntervalBytes;
    std::atomic_size_t sampleBytesAllocated{0};

    bool logGeneralStats = true;  // first time only
    stdx::unordered_map<uint32_t, StackInfo*> stackInfoMap;

    // In order to reduce load on ftdc we track the stacks we deem important enough to emit
    // once a stack is deemed "important" it remains important from that point on.
    // "Important" is a sticky quality to improve the stability of the set of stacks we emit,
    // and we always emit them in stackNum order, greatly improving ftdc compression efficiency.
    std::set<StackInfo*, ByStackNum> importantStacks;

    int numImportantSamples = 0;  // samples currently included in importantStacks
};
#endif  // MONGO_CONFIG_TCMALLOC_GOOGLE
}  // namespace heap_profiler_detail_tcmalloc

#if defined(MONGO_CONFIG_TCMALLOC_GOOGLE)
using heap_profiler_detail_tcmalloc::HeapProfiler;
#elif defined(MONGO_CONFIG_TCMALLOC_GPERF)
using heap_profiler_detail_gperf_tcmalloc::HeapProfiler;
#endif

class HeapProfilerServerStatusSection final : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return HeapProfilingEnabled;
    }

    BSONObj generateSection(OperationContext*, const BSONElement&) const override {
        BSONObjBuilder builder;
        HeapProfiler::generateServerStatusSection(builder);
        return builder.obj();
    }
};

#ifdef MONGO_HAVE_HEAP_PROFILER

auto& heapProfilerServerStatusSection =
    *ServerStatusSectionBuilder<HeapProfilerServerStatusSection>("heapProfile");

MONGO_INITIALIZER_GENERAL(StartHeapProfiling, ("EndStartupOptionHandling"), ("default"))
(InitializerContext*) {
    if (HeapProfilingEnabled)
        HeapProfiler::start();
}

#endif  // MONGO_HAVE_HEAP_PROFILER

}  // namespace
}  // namespace mongo

#endif  //_POSIX_VERSION
