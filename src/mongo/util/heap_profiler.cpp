/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/base/static_assert.h"
#include "mongo/config.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/log.h"
#include "mongo/util/stacktrace.h"

#include <gperftools/malloc_hook.h>
#include <third_party/murmurhash3/MurmurHash3.h>

// for dlfcn.h and backtrace
#if defined(_POSIX_VERSION) && defined(MONGO_CONFIG_HAVE_EXECINFO_BACKTRACE)

#include <dlfcn.h>
#include <execinfo.h>

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

namespace mongo {
namespace {

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

using Hash = uint32_t;

template <class Key, class Value>
class HashTable {
    MONGO_DISALLOW_COPYING(HashTable);

private:
    struct Entry {
        Key key{};
        Value value{};
        std::atomic<Entry*> next{nullptr};  // NOLINT
        std::atomic<bool> valid{false};     // NOLINT
        Entry() {}
    };

    const size_t maxEntries;        // we allocate storage for this many entries on creation
    std::atomic_size_t numEntries;  // number of entries currently in use  NOLINT
    size_t numBuckets;              // number of buckets, computed as numEntries * loadFactor

    // pre-allocate buckets and entries
    std::unique_ptr<std::atomic<Entry*>[]> buckets;  // NOLINT
    std::unique_ptr<Entry[]> entries;

    std::atomic_size_t nextEntry;  // first entry that's never been used  NOLINT
    Entry* freeEntry;              // linked list of entries returned to us by removeEntry

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
        if (entry) {
            entry->next = buckets[hash].load();
            buckets[hash] = entry;
            entry->key = key;
            entry->value = value;
            entry->valid = true;  // signal that the entry is well-formed and may be traversed
            numEntries++;
            return &entry->value;
        } else {
            return nullptr;
        }
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
                numEntries--;
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
    void forEach(F f) {
        for (size_t i = 0; i < nextEntry; i++) {
            Entry& entry = entries[i];
            if (entry.valid)  // only traverse well-formed entries
                f(entry.key, entry.value);
        }
    }

    // Determines whether the specified hash bucket is empty. May be called concurrently with
    // insert() and remove(). Concurrent visibility on other threads is guaranteed because
    // buckets[hash] is atomic.
    bool isEmptyBucket(Hash hash) {
        hash %= numBuckets;
        return buckets[hash] == nullptr;
    }

    // Number of entries.
    size_t size() {
        return numEntries;
    }

    // Highwater mark of number of entries used, for reporting stats.
    size_t maxSizeSeen() {
        return nextEntry;
    }

    // Returns total allocated size of the hash table, for reporting stats.
    size_t memorySizeBytes() {
        return numBuckets * sizeof(buckets[0]) + maxEntries * sizeof(entries[0]);
    }
};


class HeapProfiler {
private:
    // 0: sampling internally disabled
    // 1: sample every allocation - byte accurate but slow and big
    // >1: sample ever sampleIntervalBytes bytes allocated - less accurate but fast and small
    std::atomic_size_t sampleIntervalBytes;  // NOLINT

    stdx::mutex hashtable_mutex;  // guards updates to both object and stack hash tables
    stdx::mutex stackinfo_mutex;  // guards against races updating the StackInfo bson representation

    // cumulative bytes allocated - determines when samples are taken
    std::atomic_size_t bytesAllocated{0};  // NOLINT

    // estimated currently active bytes - sum of activeBytes for all stacks
    size_t totalActiveBytes = 0;

    //
    // Hash table of stacks
    //

    using FrameInfo = void*;  // per-frame information is just the IP

    static const int kMaxStackInfos = 20000;         // max number of unique call sites we handle
    static const int kStackHashTableLoadFactor = 2;  // keep loading <50%
    static const int kMaxFramesPerStack = 100;       // max depth of stack

    // stack HashTable Key
    struct Stack {
        int numFrames = 0;
        std::array<FrameInfo, kMaxFramesPerStack> frames;
        Stack() {}

        bool operator==(const Stack& that) {
            return this->numFrames == that.numFrames &&
                std::equal(frames.begin(), frames.begin() + numFrames, that.frames.begin());
        }

        Hash hash() {
            Hash hash;
            MONGO_STATIC_ASSERT_MSG(sizeof(frames) == sizeof(FrameInfo) * kMaxFramesPerStack,
                                    "frames array is not dense");
            MurmurHash3_x86_32(frames.data(), numFrames * sizeof(FrameInfo), 0, &hash);
            return hash;
        }
    };

    // Stack HashTable Value.
    struct StackInfo {
        int stackNum = 0;        // used for stack short name
        BSONObj stackObj{};      // symbolized representation
        size_t activeBytes = 0;  // number of live allocated bytes charged to this stack
        explicit StackInfo(int stackNum) : stackNum(stackNum) {}
        StackInfo() {}
    };

    // The stack HashTable itself.
    HashTable<Stack, StackInfo> stackHashTable{kMaxStackInfos, kStackHashTableLoadFactor};

    // frames to skip at top and bottom of backtrace when reporting stacks
    int skipStartFrames = 0;
    int skipEndFrames = 0;


    //
    // Hash table of allocated objects.
    //

    static const int kMaxObjInfos = 1024 * 1024;   // maximum tracked allocations
    static const int kObjHashTableLoadFactor = 4;  // keep hash table loading <25%

    // Obj HashTable Key.
    struct Obj {
        const void* objPtr = nullptr;
        explicit Obj(const void* objPtr) : objPtr(objPtr) {}
        Obj() {}

        bool operator==(const Obj& that) {
            return this->objPtr == that.objPtr;
        }

        Hash hash() {
            Hash hash = 0;
            MurmurHash3_x86_32(&objPtr, sizeof(objPtr), 0, &hash);
            return hash;
        }
    };

    // Obj HashTable Value.
    struct ObjInfo {
        size_t accountedLen = 0;
        StackInfo* stackInfo = nullptr;
        ObjInfo(size_t accountedLen, StackInfo* stackInfo)
            : accountedLen(accountedLen), stackInfo(stackInfo) {}
        ObjInfo() {}
    };

    // The obj HashTable itself.
    HashTable<Obj, ObjInfo> objHashTable{kMaxObjInfos, kObjHashTableLoadFactor};


    // If we encounter an error that doesn't allow us to proceed, for
    // example out of space for new hash table entries, we internally
    // disable profiling and then log an error message.
    void disable(const char* msg) {
        sampleIntervalBytes = 0;
        log() << msg;
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
        tempStack.numFrames = backtrace(tempStack.frames.data(), kMaxFramesPerStack);

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
    void generateStackIfNeeded(Stack& stack, StackInfo& stackInfo) {
        if (!stackInfo.stackObj.isEmpty())
            return;
        BSONArrayBuilder builder;
        for (int j = skipStartFrames; j < stack.numFrames - skipEndFrames; j++) {
            Dl_info dli;
            StringData frameString;
            char* demangled = nullptr;
            if (dladdr(stack.frames[j], &dli)) {
                if (dli.dli_sname) {
                    int status;
                    demangled = abi::__cxa_demangle(dli.dli_sname, 0, 0, &status);
                    if (demangled) {
                        // strip off function parameters as they are very verbose and not useful
                        char* p = strchr(demangled, '(');
                        if (p)
                            frameString = StringData(demangled, p - demangled);
                        else
                            frameString = demangled;
                    } else {
                        frameString = dli.dli_sname;
                    }
                }
            }
            if (frameString.empty()) {
                std::ostringstream s;
                s << stack.frames[j];
                frameString = s.str();
            }
            builder.append(frameString);
            if (demangled)
                free(demangled);
        }
        stackInfo.stackObj = builder.obj();
        log() << "heapProfile stack" << stackInfo.stackNum << ": " << stackInfo.stackObj;
    }

    //
    // Generate serverStatus section.
    //

    bool logGeneralStats = true;  // first time only

    // In order to reduce load on ftdc we track the stacks we deem important enough to emit
    // once a stack is deemed "important" it remains important from that point on.
    // "Important" is a sticky quality to improve the stability of the set of stacks we emit,
    // and we always emit them in stackNum order, greatly improving ftdc compression efficiency.
    std::set<StackInfo*, bool (*)(StackInfo*, StackInfo*)> importantStacks{
        [](StackInfo* a, StackInfo* b) -> bool { return a->stackNum < b->stackNum; }};

    int numImportantSamples = 0;                // samples currently included in importantStacks
    const int kMaxImportantSamples = 4 * 3600;  // reset every 4 hours at default 1 sample / sec

    void _generateServerStatusSection(BSONObjBuilder& builder) {
        // compute and log some informational stats first time through
        if (logGeneralStats) {
            const size_t maxActiveMemory = sampleIntervalBytes * kMaxObjInfos;
            const size_t objTableSize = objHashTable.memorySizeBytes();
            const size_t stackTableSize = stackHashTable.memorySizeBytes();
            const double MB = 1024 * 1024;
            log() << "sampleIntervalBytes " << sampleIntervalBytesParameter << "; "
                  << "maxActiveMemory " << maxActiveMemory / MB << " MB; "
                  << "objTableSize " << objTableSize / MB << " MB; "
                  << "stackTableSize " << stackTableSize / MB << " MB";
            // print a stack trace to log somap for post-facto symbolization
            log() << "following stack trace is for heap profiler informational purposes";
            printStackTrace();
            logGeneralStats = false;
        }

        // Stats subsection.
        BSONObjBuilder statsBuilder(builder.subobjStart("stats"));
        statsBuilder.appendNumber("totalActiveBytes", totalActiveBytes);
        statsBuilder.appendNumber("bytesAllocated", bytesAllocated);
        statsBuilder.appendNumber("numStacks", stackHashTable.size());
        statsBuilder.appendNumber("currentObjEntries", objHashTable.size());
        statsBuilder.appendNumber("maxObjEntriesUsed", objHashTable.maxSizeSeen());
        statsBuilder.doneFast();

        // Guard against races updating the StackInfo bson representation.
        stdx::lock_guard<stdx::mutex> lk(stackinfo_mutex);

        // Traverse stackHashTable accumulating potential stacks to emit.
        // We do this traversal without locking hashtable_mutex because we need to use the heap.
        // forEach guarantees this is safe wrt to insert(), and we never call remove().
        // We use stackinfo_mutex to ensure safety wrt concurrent updates to the StackInfo objects.
        // We can get skew between entries, which is ok.
        std::vector<StackInfo*> stackInfos;
        stackHashTable.forEach([&](Stack& stack, StackInfo& stackInfo) {
            if (stackInfo.activeBytes) {
                generateStackIfNeeded(stack, stackInfo);
                stackInfos.push_back(&stackInfo);
            }
        });

        // Sort the stacks and find enough stacks to account for at least 99% of the active bytes
        // deem any stack that has ever met this criterion as "important".
        auto sortByActiveBytes = [](StackInfo* a, StackInfo* b) -> bool {
            return a->activeBytes > b->activeBytes;
        };
        std::stable_sort(stackInfos.begin(), stackInfos.end(), sortByActiveBytes);
        size_t threshold = totalActiveBytes * 0.99;
        size_t cumulative = 0;
        for (auto it = stackInfos.begin(); it != stackInfos.end(); ++it) {
            StackInfo* stackInfo = *it;
            importantStacks.insert(stackInfo);
            cumulative += stackInfo->activeBytes;
            if (cumulative > threshold)
                break;
        }

        // Build the stacks subsection by emitting the "important" stacks.
        BSONObjBuilder stacksBuilder(builder.subobjStart("stacks"));
        for (auto it = importantStacks.begin(); it != importantStacks.end(); ++it) {
            StackInfo* stackInfo = *it;
            std::ostringstream shortName;
            shortName << "stack" << stackInfo->stackNum;
            BSONObjBuilder stackBuilder(stacksBuilder.subobjStart(shortName.str()));
            stackBuilder.appendNumber("activeBytes", stackInfo->activeBytes);
            stackBuilder.append("stack", stackInfo->stackObj);
        }
        stacksBuilder.doneFast();

        // importantStacks grows monotonically, so it can accumulate unneeded stacks,
        // so we clear it periodically.
        if (++numImportantSamples >= kMaxImportantSamples) {
            log() << "clearing importantStacks";
            importantStacks.clear();
            numImportantSamples = 0;
        }
    }

    //
    // Static hooks to give to the allocator.
    //

    static void alloc(const void* obj, size_t objLen) {
        heapProfiler->_alloc(obj, objLen);
    }

    static void free(const void* obj) {
        heapProfiler->_free(obj);
    }

public:
    static HeapProfiler* heapProfiler;
    static bool enabledParameter;
    static long long sampleIntervalBytesParameter;

    HeapProfiler() {
        // Set sample interval from the parameter.
        sampleIntervalBytes = sampleIntervalBytesParameter;

        // This is our only allocator dependency - ifdef and change as
        // appropriate for other allocators, using hooks or shims.
        // For tcmalloc we skip two frames that are internal to the allocator
        // so that the top frame is the public tc_* function.
        skipStartFrames = 2;
        skipEndFrames = 0;
        MallocHook::AddNewHook(alloc);
        MallocHook::AddDeleteHook(free);
    }

    static void generateServerStatusSection(BSONObjBuilder& builder) {
        if (heapProfiler)
            heapProfiler->_generateServerStatusSection(builder);
    }
};

//
// serverStatus section
//

class HeapProfilerServerStatusSection final : public ServerStatusSection {
public:
    HeapProfilerServerStatusSection() : ServerStatusSection("heapProfile") {}

    bool includeByDefault() const override {
        return HeapProfiler::enabledParameter;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder builder;
        HeapProfiler::generateServerStatusSection(builder);
        return builder.obj();
    }
} heapProfilerServerStatusSection;

//
// startup
//

HeapProfiler* HeapProfiler::heapProfiler;
bool HeapProfiler::enabledParameter = false;
long long HeapProfiler::sampleIntervalBytesParameter = 256 * 1024;

ExportedServerParameter<bool, ServerParameterType::kStartupOnly> heapProfilingEnabledParameter(
    ServerParameterSet::getGlobal(), "heapProfilingEnabled", &HeapProfiler::enabledParameter);

ExportedServerParameter<long long, ServerParameterType::kStartupOnly>
    heapProfilingSampleIntervalBytes(ServerParameterSet::getGlobal(),
                                     "heapProfilingSampleIntervalBytes",
                                     &HeapProfiler::sampleIntervalBytesParameter);

MONGO_INITIALIZER_GENERAL(StartHeapProfiling, ("EndStartupOptionHandling"), ("default"))
(InitializerContext* context) {
    if (HeapProfiler::enabledParameter)
        HeapProfiler::heapProfiler = new HeapProfiler();
    return Status::OK();
}

}  // namespace
}  // namespace mongo

#endif  // MONGO_HAVE_HEAP_PROFILER
