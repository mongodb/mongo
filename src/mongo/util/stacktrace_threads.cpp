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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/util/stacktrace.h"

#if defined(MONGO_STACKTRACE_CAN_DUMP_ALL_THREADS)

#include <array>
#include <atomic>
#include <boost/filesystem.hpp>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <fmt/format.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "mongo/base/parse_number.h"
#include "mongo/base/string_data.h"
#include "mongo/config.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/log.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/stacktrace_json.h"
#include "mongo/util/stacktrace_somap.h"

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
        log() << "CachedMetaGenerator: " << _hits << "/" << (_hits + _misses);
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
    /** if `redactAddr`, suppress any raw address fields, consistent with ASLR. */
    void printStacks(StackTraceSink& sink, bool redactAddrs = true);

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

    void messageToJson(CheapJson::Value& jsonThreads,
                       const ThreadBacktrace& msg,
                       bool redact,
                       CachedMetaGenerator* metaGen);

    void printAllThreadStacksFormat(StackTraceSink& sink,
                                    const std::vector<ThreadBacktrace*>& received,
                                    const std::vector<int>& missedTids,
                                    bool redactAddrs);

    ThreadBacktrace* acquireBacktraceBuffer() {
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

    void postBacktrace(ThreadBacktrace* msg) {
        _stackCollection.load()->results.push(msg);
    }

    int _signal = 0;
    std::atomic<int> _processingTid = -1;                               // NOLINT
    std::atomic<StackCollectionOperation*> _stackCollection = nullptr;  // NOLINT

    MONGO_STATIC_ASSERT(decltype(_processingTid)::is_always_lock_free);
    MONGO_STATIC_ASSERT(decltype(_stackCollection)::is_always_lock_free);
};

// TODO: Format is still TBD here.
void State::printAllThreadStacksFormat(StackTraceSink& sink,
                                       const std::vector<ThreadBacktrace*>& received,
                                       const std::vector<int>& missedTids,
                                       bool redactAddrs) {
    CheapJson jsonEnv{sink};
    auto jsonDoc = jsonEnv.doc();
    jsonDoc.setPretty(true);
    auto jsonRootObj = jsonDoc.appendObj();
    CachedMetaGenerator metaGen;
    {
        auto jsonThreadInfoKey = jsonRootObj.appendKey("threadInfo");
        auto jsonThreadInfoArr = jsonThreadInfoKey.appendArr();

        // Missed threads are minimally represented with just a "tid" field.
        for (int tid : missedTids) {
            auto obj = jsonThreadInfoArr.appendObj();
            obj.appendKey("tid").append(tid);
        }

        for (ThreadBacktrace* message : received) {
            messageToJson(jsonThreadInfoArr, *message, redactAddrs, &metaGen);
        }
    }
    // `metaGen` remembers all the bases from which it ever produced a frame.
    // We can use it to do somap filtering.
    {
        auto jsonProcInfoKey = jsonRootObj.appendKey("processInfo");
        auto jsonProcInfo = jsonProcInfoKey.appendObj();
        const BSONObj& bsonProcInfo = globalSharedObjectMapInfo().obj();
        for (const BSONElement& be : bsonProcInfo) {
            // special case handling for the `somap` array.
            // Everything else is passed through.
            StringData key = be.fieldNameStringData();
            if (be.type() == BSONType::Array && key == "somap"_sd) {
                auto jsonSoMapKey = jsonProcInfo.appendKey(key);
                auto jsonSoMap = jsonSoMapKey.appendArr();
                for (const BSONElement& ae : be.Array()) {
                    BSONObj bRec = ae.embeddedObject();
                    uintptr_t soBase = Hex::fromHex(bRec.getStringField("b"));
                    if (const auto* file = metaGen.findFile(soBase); file != nullptr) {
                        StringData id = file->id();
                        // Irrelevant somap entries are omitted.
                        // Remove the base addr "b" and add the file's "id".
                        // Everything else is passed through.
                        auto jsonSoMapElemObj = jsonSoMap.appendObj();
                        jsonSoMapElemObj.setPretty(false);
                        jsonSoMapElemObj.appendKey("id"_sd).append(file->id());
                        for (auto&& be : bRec) {
                            if (be.fieldNameStringData() == "b"_sd)
                                continue;
                            jsonSoMapElemObj.append(be);
                        }
                    }
                }
            } else {
                jsonProcInfo.append(be);
            }
        }
    }
}

void State::printStacks(StackTraceSink& sink, bool redactAddrs) {
    std::set<int> pendingTids;
    std::vector<int> missedTids;
    iterateTids([&](int tid) { pendingTids.insert(tid); });
    log() << "Preparing to dump up to " << pendingTids.size() << " thread stacks";

    // Make a StackCollectionOperation and load it with enough ThreadBacktrace buffers to serve
    // every pending thread.
    StackCollectionOperation collection;
    std::vector<ThreadBacktrace> messageStorage(pendingTids.size());
    for (auto& m : messageStorage)
        collection.pool.push(&m);
    _stackCollection.store(&collection, std::memory_order_release);

    for (auto iter = pendingTids.begin(); iter != pendingTids.end();) {
        errno = 0;
        if (int r = tgkill(getpid(), *iter, _signal); r < 0) {
            int errsv = errno;
            log() << "failed to signal thread (" << *iter << "):" << strerror(errsv);
            missedTids.push_back(*iter);
            iter = pendingTids.erase(iter);
        } else {
            ++iter;
        }
    }
    log() << "signalled " << pendingTids.size() << " threads";

    std::vector<ThreadBacktrace*> received;
    received.reserve(pendingTids.size());

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
    printAllThreadStacksFormat(sink, received, missedTids, redactAddrs);
}

void State::messageToJson(CheapJson::Value& jsonThreads,
                          const ThreadBacktrace& msg,
                          bool redact,
                          CachedMetaGenerator* metaGen) {
    auto jsonThreadObj = jsonThreads.appendObj();
    if (auto threadName = readThreadName(msg.tid); !threadName.empty())
        jsonThreadObj.appendKey("name").append(threadName);
    jsonThreadObj.appendKey("tid").append(msg.tid);
    auto jsonFrames = jsonThreadObj.appendKey("backtrace").appendArr();

    for (void* const addrPtr : msg.addrRange()) {
        const uintptr_t addr = reinterpret_cast<uintptr_t>(addrPtr);
        auto jsonFrame = jsonFrames.appendObj();
        jsonFrame.setPretty(false);  // Compactly print one frame object per line.
        if (!redact)
            jsonFrame.appendKey("a").append(Hex(addr));
        const auto& meta = metaGen->load(addrPtr);
        if (const auto& mf = meta.file(); mf) {
            jsonFrame.appendKey("id").append(mf->id());
            jsonFrame.appendKey("f").append(mf->path().filename().native());
            if (!redact)
                jsonFrame.appendKey("bAddr").append(Hex(mf->base()));
            jsonFrame.appendKey("o").append(Hex(addr - mf->base()));
        }
        if (const auto& sym = meta.symbol(); sym) {
            jsonFrame.appendKey("s").append(sym->name());
            jsonFrame.appendKey("sOffset").append(Hex(addr - sym->base()));
        }
    }
}

void State::action(siginfo_t* si) {
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

/**
 * Called from single-thread init time. The stack tracer will use the specified `signal`.
 */
void initialize(int signal) {
    stateSingleton->setSignal(signal);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = [](int, siginfo_t* si, void*) { stateSingleton->action(si); };
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (sigaction(signal, &sa, nullptr) != 0) {
        int savedErr = errno;
        severe() << format(FMT_STRING("Failed to install sigaction for signal {} ({})"),
                           signal,
                           strerror(savedErr));
        fassertFailed(31376);
    }
}

}  // namespace
}  // namespace stack_trace_detail


void printAllThreadStacks(StackTraceSink& sink) {
    stack_trace_detail::stateSingleton->printStacks(sink);
}

void setupStackTraceSignalAction(int signal) {
    stack_trace_detail::initialize(signal);
}

void markAsStackTraceProcessingThread() {
    stack_trace_detail::stateSingleton->markProcessingThread();
}

}  // namespace mongo
#endif  // !defined(MONGO_STACKTRACE_CAN_DUMP_ALL_THREADS)
