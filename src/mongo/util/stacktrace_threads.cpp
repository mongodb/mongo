/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include "mongo/util/stacktrace.h"

#if defined(MONGO_STACKTRACE_CAN_DUMP_ALL_THREADS)

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <atomic>
#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <fmt/format.h>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>
// IWYU pragma: no_include <syscall.h>
// IWYU pragma: no_include "bits/types/siginfo_t.h"

#include "mongo/base/parse_number.h"
#include "mongo/base/static_assert.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/future.h"
#include "mongo/util/stacktrace_somap.h"

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace stack_trace_detail {

namespace {

constexpr StringData kTaskDir = "/proc/self/task"_sd;

StringData getBaseName(StringData path) {
    size_t lastSlash = path.rfind('/');
    if (lastSlash == std::string::npos)
        return path;
    return path.substr(lastSlash + 1);
}

ptrdiff_t offsetFromBase(uintptr_t base, uintptr_t addr) {
    return addr - base;
}

class CachedMetaGenerator {
public:
    class File {
    public:
        File(uintptr_t base, std::string id, boost::filesystem::path path)
            : _base(base), _id(std::move(id)), _path(std::move(path)) {}

        uintptr_t base() const {
            return _base;
        }

        StringData id() const {
            return _id;
        }

        const boost::filesystem::path& path() const {
            return _path;
        }

    private:
        uintptr_t _base;
        std::string _id;
        boost::filesystem::path _path;
    };

    class Symbol {
    public:
        Symbol(uintptr_t base, std::string name) : _base(base), _name(std::move(name)) {}
        uintptr_t base() const {
            return _base;
        }
        StringData name() const {
            return _name;
        }

    private:
        uintptr_t _base;
        std::string _name;
    };

    class RedactedMeta {
    public:
        RedactedMeta(void* a, const File* f, const Symbol* s) : _addr(a), _file(f), _symbol(s) {}
        void* addr() const {
            return _addr;
        }
        const File* file() const {
            return _file;
        }
        const Symbol* symbol() const {
            return _symbol;
        }

    private:
        void* _addr;
        const File* _file;
        const Symbol* _symbol;
    };

    CachedMetaGenerator() = default;

    ~CachedMetaGenerator() {
        LOGV2(23393,
              "CachedMetaGenerator: {hits}/{hitsAndMisses}",
              "CachedMetaGenerator",
              "hits"_attr = _hits,
              "hitsAndMisses"_attr = (_hits + _misses));
    }

    const RedactedMeta& load(void* addr) {
        auto it = _cache.find(addr);
        if (it != _cache.end()) {
            ++_hits;
            return it->second;
        }
        ++_misses;
        const auto& rawMeta = _gen.load(addr);

        const File* file{};
        if (const auto& rmf = rawMeta.file(); rmf) {
            auto it = _files.find(rmf.base());
            if (it == _files.end()) {
                it = _files
                         .insert({rmf.base(),
                                  File{rmf.base(),
                                       makeId(),
                                       boost::filesystem::path{std::string{rmf.name()}}}})
                         .first;
            }
            file = &it->second;
        }

        const Symbol* sym{};
        if (const auto& rms = rawMeta.symbol(); rms) {
            auto it = _symbols.find(rms.base());
            if (it == _symbols.end())
                it = _symbols.insert({rms.base(), {rms.base(), std::string{rms.name()}}}).first;
            sym = &it->second;
        }

        return _cache.insert({addr, {addr, file, sym}}).first->second;
    }

    const File* findFile(uintptr_t soBase) const {
        if (auto it = _files.find(soBase); it != _files.end())
            return &it->second;
        return nullptr;
    }

private:
    std::string makeId() {
        return format(FMT_STRING("{:03d}"), _serial++);
    }

    size_t _hits = 0;
    size_t _misses = 0;
    size_t _serial = 0;
    std::map<uintptr_t, File> _files;
    std::map<uintptr_t, Symbol> _symbols;
    stdx::unordered_map<void*, RedactedMeta> _cache;
    StackTraceAddressMetadataGenerator _gen;
};

/** Safe to call from a signal handler. Might wake early with EINTR. */
void sleepMicros(int64_t usec) {
    auto nsec = usec * 1000;
    constexpr static int64_t k1E9 = 1'000'000'000;
    timespec ts{nsec / k1E9, nsec % k1E9};
    nanosleep(&ts, nullptr);
}

int gettid() {
    return syscall(SYS_gettid);
}

int tgkill(int pid, int tid, int sig) {
    return syscall(SYS_tgkill, pid, tid, sig);
}

boost::filesystem::path taskDir() {
    return boost::filesystem::path("/proc/self/task");
}

/** Call `f(tid)` on each thread `tid` in this process except the calling thread. */
template <typename F>
void iterateTids(F&& f) {
    int selfTid = gettid();
    auto iter = boost::filesystem::directory_iterator{taskDir()};
    for (const auto& entry : iter) {
        int tid;
        if (!NumberParser{}(entry.path().filename().string(), &tid).isOK())
            continue;  // Ignore non-integer names (e.g. "." or "..").
        if (tid == selfTid)
            continue;  // skip the current thread
        f(tid);
    }
}

bool tidExists(int tid) {
    return exists(taskDir() / std::to_string(tid));
}

std::string readThreadName(int tid) {
    std::string threadName;
    try {
        boost::filesystem::ifstream in(taskDir() / std::to_string(tid) / "comm");
        std::getline(in, threadName);
    } catch (...) {
    }
    return threadName;
}

/** Cannot yield. AS-Safe. */
class SimpleSpinLock {
public:
    void lock() {
        while (true) {
            for (int i = 0; i < 100; ++i) {
                if (!_flag.test_and_set(std::memory_order_acquire)) {
                    return;
                }
            }
            sleepMicros(1);
        }
    }

    void unlock() {
        _flag.clear(std::memory_order_release);
    }

private:
    std::atomic_flag _flag = ATOMIC_FLAG_INIT;  // NOLINT
};

template <typename T>
class AsyncStack {
public:
    T* tryPop() {
        stdx::lock_guard lock{_spin};
        T* node = _head;
        if (node) {
            node = std::exchange(_head, node->intrusiveNext);
            node->intrusiveNext = nullptr;
        }
        return node;
    }

    void push(T* node) {
        stdx::lock_guard lock{_spin};
        node->intrusiveNext = std::exchange(_head, node);
    }

private:
    T* _head = nullptr;
    SimpleSpinLock _spin;  // guards _head
};

class ThreadBacktrace {
public:
    ThreadBacktrace* intrusiveNext;

    static const size_t capacity = kStackTraceFrameMax;

    auto addrRange() const {
        struct AddrRange {
            void **b, **e;
            void** begin() const {
                return b;
            }
            void** end() const {
                return e;
            }
        };
        return AddrRange{addrs, addrs + size};
    }

    int tid = 0;
    std::unique_ptr<void*[]> addrStorage = std::make_unique<void*[]>(capacity);
    void** addrs = addrStorage.get();
    size_t size = 0;
};

class State {
public:
    void printStacks(StackTraceSink& sink);
    void printStacks();
    void printAllThreadStacksBlocking();

    /**
     * We need signals for two purpposes in the stack tracing system.
     *
     * An external process sends a signal to initiate stack tracing.  When that's received,
     * we *also* need a signal to send to each thread to cause to dump its backtrace.
     * The `siginfo` provides enough information to allow one signal to serve both roles.
     *
     * Since all threads are open to receiving this signal, any of them can be selected to
     * receive it when it comes from outside. So we arrange for any thread that receives the
     * undirected stack trace signal to re-issue it directy at the signal processing thread.
     *
     * The signal processing thread will have the signal blocked, and handle it
     * synchronously with sigwaitinfo, so this handler only applies to the other
     * respondents.
     */
    void action(siginfo_t* si);

    void markProcessingThread() {
        _processingTid.store(gettid(), std::memory_order_release);
    }

    void setSignal(int signal) {
        _signal = signal;
    }

private:
    /** An in-flight all-thread stack collection. */
    struct StackCollectionOperation {
        AsyncStack<ThreadBacktrace> pool;
        AsyncStack<ThreadBacktrace> results;
    };

    struct AbstractEmitter {
        virtual void open() = 0;
        virtual void prologue(const BSONObj& obj) = 0;
        virtual void threadRecordsOpen() = 0;
        virtual void threadRecord(const BSONObj& obj) = 0;
        virtual void threadRecordsClose() = 0;
        virtual void close() = 0;
    };

    /** Write several log statements, one per thread. */
    void printToEmitter(AbstractEmitter& emitter);

    void collectStacks(std::vector<ThreadBacktrace>& messageStorage,
                       std::vector<ThreadBacktrace*>& received,
                       std::vector<int>& missedTids);

    ThreadBacktrace* acquireBacktraceBuffer();

    void postBacktrace(ThreadBacktrace* msg) {
        _stackCollection.load()->results.push(msg);
    }

    int _signal = 0;
    std::atomic<int> _processingTid = -1;                               // NOLINT
    std::atomic<StackCollectionOperation*> _stackCollection = nullptr;  // NOLINT
    PrintAllStacksSession _printAllStacksSession;

    MONGO_STATIC_ASSERT(decltype(_processingTid)::is_always_lock_free);
    MONGO_STATIC_ASSERT(decltype(_stackCollection)::is_always_lock_free);
};

ThreadBacktrace* State::acquireBacktraceBuffer() {
    while (true) {
        auto coll = _stackCollection.load();
        if (!coll) {
            // Brute sanity check. Should not really happen.
            // A raise(SIGUSR2) could cause it, but we aren't supporting that.
            return nullptr;
        }
        if (ThreadBacktrace* msg = coll->pool.tryPop(); msg != nullptr)
            return msg;
        sleepMicros(1);
    }
}

void State::collectStacks(std::vector<ThreadBacktrace>& messageStorage,
                          std::vector<ThreadBacktrace*>& received,
                          std::vector<int>& missedTids) {
    std::set<int> pendingTids;
    iterateTids([&](int tid) { pendingTids.insert(tid); });
    LOGV2(23394,
          "Preparing to dump up to {numThreads} thread stacks",
          "Preparing to dump thread stacks",
          "numThreads"_attr = pendingTids.size());

    messageStorage.resize(pendingTids.size());
    received.reserve(pendingTids.size());

    // Make a StackCollectionOperation and load it with enough ThreadBacktrace buffers to serve
    // every pending thread.
    StackCollectionOperation collection;
    for (auto& m : messageStorage)
        collection.pool.push(&m);
    _stackCollection.store(&collection, std::memory_order_release);

    for (auto iter = pendingTids.begin(); iter != pendingTids.end();) {
        errno = 0;
        if (int r = tgkill(getpid(), *iter, _signal); r < 0) {
            int errsv = errno;
            LOGV2(23395,
                  "Failed to signal thread ({tid}): {error}",
                  "Failed to signal thread",
                  "tid"_attr = *iter,
                  "error"_attr = strerror(errsv));
            missedTids.push_back(*iter);
            iter = pendingTids.erase(iter);
        } else {
            ++iter;
        }
    }
    LOGV2(23396,
          "Signalled {numThreads} threads",
          "Signalled threads",
          "numThreads"_attr = pendingTids.size());

    size_t napMicros = 0;
    while (!pendingTids.empty()) {
        if (ThreadBacktrace* message = collection.results.tryPop(); message) {
            napMicros = 0;
            if (pendingTids.erase(message->tid) != 0) {
                received.push_back(message);
            } else {
                collection.pool.push(message);
            }
        } else if (napMicros < 50'000) {
            // Results queue is dry and we haven't napped enough to justify a reap.
            napMicros += 1'000;
            sleepMicros(1'000);
        } else {
            napMicros = 0;
            // Prune dead threads from the pendingTids set before retrying.
            for (auto iter = pendingTids.begin(); iter != pendingTids.end();) {
                if (!tidExists(*iter)) {
                    missedTids.push_back(*iter);
                    iter = pendingTids.erase(iter);
                } else {
                    ++iter;
                }
            }
        }
    }
    _stackCollection.store(nullptr, std::memory_order_release);
}


void State::printStacks(StackTraceSink& sink) {
    struct SinkEmitter : public AbstractEmitter {
        explicit SinkEmitter(StackTraceSink& s) : _sink{s} {}
        void open() override {}
        void prologue(const BSONObj& obj) override {
            for (auto& e : obj)
                _bob.append(e);
        }
        void threadRecordsOpen() override {
            _threadRecords = std::make_unique<BSONArrayBuilder>(_bob.subarrayStart("threadInfo"));
        }
        void threadRecord(const BSONObj& obj) override {
            _threadRecords->append(obj);
        }
        void threadRecordsClose() override {
            _threadRecords->done();
        }
        void close() override {
            _sink << tojson(_bob.done(), ExtendedRelaxedV2_0_0);
        }

        StackTraceSink& _sink;
        BSONObjBuilder _bob;
        std::unique_ptr<BSONArrayBuilder> _threadRecords;
    };
    SinkEmitter emitter{sink};
    printToEmitter(emitter);
}

void State::printStacks() {
    struct LogEmitter : public AbstractEmitter {
        void open() override {
            LOGV2(31423, "===== multithread stacktrace session begin =====");
        }
        void prologue(const BSONObj& obj) override {
            LOGV2(31424,
                  "Stacktrace Prologue: {prologue}",
                  "Stacktrace Prologue",
                  "prologue"_attr = obj);
        }
        void threadRecordsOpen() override {}
        void threadRecord(const BSONObj& obj) override {
            LOGV2(31425,  //
                  "Stacktrace Record: {record}",
                  "Stacktrace Record",
                  "record"_attr = obj);
        }
        void threadRecordsClose() override {}
        void close() override {
            LOGV2(31426, "===== multithread stacktrace session end =====");
        }
    };

    LogEmitter emitter;
    auto notifier = _printAllStacksSession.notifier();
    printToEmitter(emitter);
}

void State::printAllThreadStacksBlocking() {
    auto waiter = _printAllStacksSession.waiter();
    kill(getpid(), _signal);  // The SignalHandler thread calls printAllThreadStacks.
}

void State::printToEmitter(AbstractEmitter& emitter) {
    std::vector<ThreadBacktrace> messageStorage;
    std::vector<ThreadBacktrace*> received;
    std::vector<int> missedTids;
    collectStacks(messageStorage, received, missedTids);

    CachedMetaGenerator metaGen;
    const BSONObj& bsonProcInfo = globalSharedObjectMapInfo().obj();

    // Load all addrs in all threads into the metaGen.
    for (auto&& msg : received)
        for (auto&& addrPtr : msg->addrRange())
            metaGen.load(addrPtr);

    emitter.open();
    {
        BSONObjBuilder prologue;
        if (!missedTids.empty()) {
            BSONArrayBuilder tidArray(prologue.subarrayStart("missedThreadIds"_sd));
            for (int tid : missedTids)
                tidArray.append(tid);
        }
        {
            BSONObjBuilder procInfo(prologue.subobjStart("processInfo"_sd));
            for (const BSONElement& be : bsonProcInfo) {
                StringData key = be.fieldNameStringData();

                // Handle 'somap' specially. Pass everything else through.
                if (be.type() == BSONType::Array && key == "somap"_sd) {
                    BSONArrayBuilder soMapArr(procInfo.subarrayStart(key));
                    for (const BSONElement& ae : be.Array()) {
                        BSONObj bRec = ae.embeddedObject();
                        uintptr_t soBase = Hex::fromHex(bRec.getStringField("b"_sd));

                        // Skip any files that aren't present in the metaGen.
                        const auto* file = metaGen.findFile(soBase);
                        if (file == nullptr)
                            continue;
                        BSONObjBuilder outLibrary(soMapArr.subobjStart());

                        // Replace "b" with the `file->id()`. Pass everything else through.
                        for (auto&& be : bRec) {
                            if (be.fieldNameStringData() == "b"_sd) {
                                outLibrary.append("b"_sd, file->id());
                            } else {
                                outLibrary.append(be);
                            }
                        }
                    }
                } else {
                    procInfo.append(be);
                }
            }
        }
        emitter.prologue(prologue.done());
    }
    emitter.threadRecordsOpen();
    for (ThreadBacktrace* msg : received) {
        BSONObjBuilder threadRecord;
        if (auto threadName = readThreadName(msg->tid); !threadName.empty()) {
            threadRecord.append("name"_sd, threadName);
        }
        threadRecord.append("tid"_sd, msg->tid);
        {
            BSONArrayBuilder backtrace(threadRecord.subarrayStart("backtrace"_sd));
            for (void* const addrPtr : msg->addrRange()) {
                const auto& meta = metaGen.load(addrPtr);
                const uintptr_t addr = reinterpret_cast<uintptr_t>(addrPtr);
                BSONObjBuilder frame(backtrace.subobjStart());
                if (const auto& mf = meta.file(); mf) {
                    StringData base = mf->id();  // really a made-up id string
                    frame.append("b"_sd, base);
                    frame.append("o"_sd, Hex(offsetFromBase(mf->base(), addr)));
                }
                if (const auto& sym = meta.symbol(); sym) {
                    frame.append("s"_sd, sym->name());
                    frame.append("s+"_sd, Hex(offsetFromBase(sym->base(), addr)));
                }
            }
        }
        emitter.threadRecord(threadRecord.done());
    }
    emitter.threadRecordsClose();
    emitter.close();
}

void State::action(siginfo_t* si) {
    const ScopeGuard errnoGuard([e = errno] { errno = e; });
    switch (si->si_code) {
        case SI_USER:
        case SI_QUEUE:
            // Received from outside. Forward to signal processing thread if there is one.
            if (int sigTid = _processingTid.load(std::memory_order_acquire); sigTid != -1)
                tgkill(getpid(), sigTid, si->si_signo);
            break;
        case SI_TKILL:
            // Users should call the toplevel printAllThreadStacks function.
            // An SI_TKILL could be a raise(SIGUSR2) call, and we ignore those.
            // Received from the signal processing thread.
            // Submit this thread's backtrace to the results stack.
            if (ThreadBacktrace* msg = acquireBacktraceBuffer(); msg != nullptr) {
                msg->tid = gettid();
                msg->size = rawBacktrace(msg->addrs, msg->capacity);
                postBacktrace(msg);
            }
            break;
    }
}

State* stateSingleton = new State{};
extern "C" void stateSingletonAction(int, siginfo_t* si, void*) {
    stateSingleton->action(si);
}

/**
 * Called from single-thread init time. The stack tracer will use the specified `signal`.
 */
void initialize(int signal) {
    stateSingleton->setSignal(signal);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    // We should never need to add to this lambda because it simply sets up handler
    // execution. Any changes should either be in State::action or in the signal
    // handler itself.
    sa.sa_sigaction = stateSingletonAction;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    if (sigaction(signal, &sa, nullptr) != 0) {
        int savedErr = errno;
        LOGV2_FATAL(31376,
                    "Failed to install sigaction for signal {signal}: {error}",
                    "Failed to install sigaction for signal",
                    "signal"_attr = signal,
                    "error"_attr = strerror(savedErr));
    }
}

}  // namespace

}  // namespace stack_trace_detail


void printAllThreadStacks(StackTraceSink& sink) {
    stack_trace_detail::stateSingleton->printStacks(sink);
}

void printAllThreadStacks() {
    stack_trace_detail::stateSingleton->printStacks();
}

void printAllThreadStacksBlocking() {
    stack_trace_detail::stateSingleton->printAllThreadStacksBlocking();
}

void setupStackTraceSignalAction(int signal) {
    stack_trace_detail::initialize(signal);
}

void markAsStackTraceProcessingThread() {
    stack_trace_detail::stateSingleton->markProcessingThread();
}

}  // namespace mongo
#endif  // !defined(MONGO_STACKTRACE_CAN_DUMP_ALL_THREADS)
