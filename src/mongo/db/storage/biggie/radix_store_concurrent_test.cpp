/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/storage/biggie/store.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/unittest.h"

#include <boost/thread/barrier.hpp>
#include <tuple>

namespace mongo {
namespace biggie {

// Helper fixture to run radix tree modifications in parallel on different threads. The result will
// be merged into a master tree and any merge conflicts will be retried. The fixture remembers the
// order of operations that get merged in and replays the jobs on a single thread to validate that
// it produces the same result.
template <std::size_t NumThreads>
class ConcurrentRadixStoreTest : public unittest::Test {
public:
    ConcurrentRadixStoreTest() {}
    virtual ~ConcurrentRadixStoreTest() {
        _checkValid(_store);
    }

    template <class SetupFunc>
    void setup(SetupFunc func) {
        auto [tree, version] = _head();
        func(tree);
        ASSERT_TRUE(_commit(tree, version, 0));
        _executionOrder.clear();
        _setupFunc = func;
    }

    template <class... WorkFuncs>
    void initThreads(WorkFuncs&&... funcs) {
        // Create index sequence so we can access the indices of the funcs parameter pack
        initThreadsImpl_(std::index_sequence_for<WorkFuncs...>{},
                         std::forward<WorkFuncs>(funcs)...);
    }

    template <class Rep, class Period>
    StringStore runThreads(stdx::chrono::duration<Rep, Period> dur) {
        _barrier.count_down_and_wait();
        stdx::this_thread::sleep_for(dur);
        _stop.store(true);
        for (auto& thread : _threads)
            thread.join();

        auto result = _fork();

        // Play back the same operations on a single thread in the order they executed to ensure
        // that we get the same end result
        StringStore playback;
        if (_setupFunc)
            _setupFunc(playback);
        std::array<std::size_t, NumThreads> terms;
        terms.fill(0);

        for (auto op : _executionOrder) {
            _workFuncs[op](playback, terms[op]++);
        }

        if (result != playback) {
            LOGV2_ERROR_OPTIONS(4785800,
                                {logv2::LogTruncation::Disabled},
                                "Execution order of failure",
                                "order"_attr = _executionOrder,
                                "concurrent"_attr = result.to_string_for_test(),
                                "serial"_attr = playback.to_string_for_test());
            ASSERT(false);
        }

        return result;
    }

private:
    // Wrapper for unittest work that handles thread synchronization and merging of radix trees
    template <class WorkFunc>
    class Worker {
    public:
        Worker(ConcurrentRadixStoreTest& thisTest, std::size_t threadIndex, WorkFunc func)
            : _thisTest(thisTest), _func(std::move(func)), _index(threadIndex) {}

        void operator()() {
            // Wait until all threads have reached this point before we begin
            _thisTest._barrier.count_down_and_wait();

            // Term is a counter for the number of successful commits this thread has done
            std::size_t term = 0;
            while (!_thisTest._stop.load()) {
                // Take a copy of the current master tree into base and make a working copy that we
                // apply our operations to
                StringStore base;
                uint64_t baseVersion;

                std::tie(base, baseVersion) = _thisTest._head();
                auto copy = base;
                // Perform the actual work. The work must be deterministic for every value of term.
                bool change = _func(copy, term);
                if (!change)
                    continue;

                // Try merging our tree with head of master tree, retry as long as we are not
                // merging with the latest version
                uint64_t version;
                bool committed = true;
                do {
                    StringStore head;
                    std::tie(head, version) = _thisTest._head();
                    try {
                        copy.merge3(base, head);
                    } catch (const merge_conflict_exception&) {
                        // Retry this operation in case of merge conflict
                        committed = false;
                        break;
                    }

                    // Update base to be latest that we know of
                    base = std::move(head);
                } while (!_thisTest._commit(copy, version, _index));
                if (committed) {
                    ++term;
                    _checkValid(copy);
                }
            }
        }

    private:
        ConcurrentRadixStoreTest& _thisTest;
        WorkFunc _func;
        std::size_t _index;
    };

    template <class WorkFunc>
    friend class Worker;

    template <std::size_t... Is, class... WorkFuncs>
    void initThreadsImpl_(std::index_sequence<Is...>, const WorkFuncs&... funcs) {
        // Index sequence as a helper type to retrieve indexes for the WorkFuncs while expanding the
        // parameter pack. It is a helper type with template integer template arguments
        // 'std::index_sequence<0, 1, ..., N>'. Then we expand both parameter packs simultaneously
        // and we can therefore associate each WorkerThread (and its WorkerFunc) with an index.
        _threads = {stdx::thread(Worker<WorkFuncs>(*this, Is, funcs))...};
        _workFuncs = {funcs...};
    }

    StringStore _fork() const {
        stdx::lock_guard lock(_mutex);
        return _store;
    }

    std::tuple<StringStore, uint64_t> _head() const {
        stdx::lock_guard lock(_mutex);
        return std::make_tuple(_store, _version);
    }

    bool _commit(StringStore tree, uint64_t checkedOutVersion, std::size_t threadIndex) {
        stdx::lock_guard lock(_mutex);
        if (checkedOutVersion != _version)
            return false;

        _store = std::move(tree);
        ++_version;
        _executionOrder.push_back(threadIndex);
        return true;
    }

    static void _checkValid(StringStore& store) {
        size_t actualSize = 0;
        size_t actualDataSize = 0;
        std::string lastKey = "";
        for (auto& item : store) {
            ASSERT_GT(item.first, lastKey);
            actualDataSize += item.second.size();
            actualSize++;
        }
        ASSERT_EQ(store.size(), actualSize);
        ASSERT_EQ(store.dataSize(), actualDataSize);
    }

    mutable Mutex _mutex;
    StringStore _store;
    uint64_t _version;

    std::array<stdx::thread, NumThreads> _threads;
    boost::barrier _barrier{NumThreads + 1};
    AtomicWord<bool> _stop{false};

    std::vector<int> _executionOrder;
    std::function<void(StringStore&)> _setupFunc;
    std::array<std::function<bool(StringStore&, std::size_t)>, NumThreads> _workFuncs;
};

// Helper to be be able to create a fixture with template parameters
class ConcurrentRadixStoreTestFourThreads : public ConcurrentRadixStoreTest<4> {};
class ConcurrentRadixStoreTestNineThreads : public ConcurrentRadixStoreTest<9> {};

TEST_F(ConcurrentRadixStoreTestFourThreads, UpdateDifferentKeysDifferentBranches) {
    setup([](StringStore& tree) {
        tree.insert({"a", ""});
        tree.insert({"b", ""});
        tree.insert({"c", ""});
        tree.insert({"d", ""});
    });

    std::string s1;
    std::string s2;
    std::string s3;
    std::string s4;
    initThreads(
        [&s1](StringStore& tree, std::size_t term) {
            s1 = std::string(term, 'a');
            return tree.update({"a", s1}).second;
        },
        [&s2](StringStore& tree, std::size_t term) {
            s2 = std::string(term, 'b');
            return tree.update({"b", s2}).second;
        },
        [&s3](StringStore& tree, std::size_t term) {
            s3 = std::string(term, 'c');
            return tree.update({"c", s3}).second;
        },
        [&s4](StringStore& tree, std::size_t term) {
            s4 = std::string(term, 'd');
            return tree.update({"d", s4}).second;
        });
    auto result = runThreads(stdx::chrono::seconds(3));
    ASSERT_EQ(result.find("a")->second, s1);
    ASSERT_EQ(result.find("b")->second, s2);
    ASSERT_EQ(result.find("c")->second, s3);
    ASSERT_EQ(result.find("d")->second, s4);
}


TEST_F(ConcurrentRadixStoreTestFourThreads, UpdateDifferentKeysSameBranch) {
    setup([](StringStore& tree) {
        tree.insert({"a", ""});
        tree.insert({"aa", ""});
        tree.insert({"aaa", ""});
        tree.insert({"aaaa", ""});
    });

    std::string s1;
    std::string s2;
    std::string s3;
    std::string s4;
    initThreads(
        [&s1](StringStore& tree, std::size_t term) {
            s1 = std::string(term, 'a');
            return tree.update({"a", s1}).second;
        },
        [&s2](StringStore& tree, std::size_t term) {
            s2 = std::string(term, 'b');
            return tree.update({"aa", s2}).second;
        },
        [&s3](StringStore& tree, std::size_t term) {
            s3 = std::string(term, 'c');
            return tree.update({"aaa", s3}).second;
        },
        [&s4](StringStore& tree, std::size_t term) {
            s4 = std::string(term, 'd');
            return tree.update({"aaaa", s4}).second;
        });
    auto result = runThreads(stdx::chrono::seconds(3));
    ASSERT_EQ(result.find("a")->second, s1);
    ASSERT_EQ(result.find("aa")->second, s2);
    ASSERT_EQ(result.find("aaa")->second, s3);
    ASSERT_EQ(result.find("aaaa")->second, s4);
}

TEST_F(ConcurrentRadixStoreTestFourThreads, UpdateSameKey) {
    setup([](StringStore& tree) { tree.insert({"key", ""}); });

    initThreads(
        [](StringStore& tree, std::size_t term) {
            return tree.update({"key", "a"}).second;
        },
        [](StringStore& tree, std::size_t term) {
            return tree.update({"key", "b"}).second;
        },
        [](StringStore& tree, std::size_t term) {
            return tree.update({"key", "c"}).second;
        },
        [](StringStore& tree, std::size_t term) {
            return tree.update({"key", "d"}).second;
        });
    auto result = runThreads(stdx::chrono::seconds(3));
    auto res = result.find("key")->second;
    ASSERT(std::string("abcd").find(res) != std::string::npos);
}

TEST_F(ConcurrentRadixStoreTestFourThreads, InsertUpdateEraseSameKey) {
    initThreads([](StringStore& tree, std::size_t term) { return tree.erase("key"); },
                [](StringStore& tree, std::size_t term) {
                    return tree.insert({"key", "a"}).second;
                },
                [](StringStore& tree, std::size_t term) {
                    return tree.update({"key", "b"}).second;
                },
                [](StringStore& tree, std::size_t term) {
                    return tree.update({"key", "c"}).second;
                });
    auto result = runThreads(stdx::chrono::seconds(3));
    if (auto res = result.find("key"); res != result.end()) {
        ASSERT(std::string("abc").find(res->second) != std::string::npos);
    }
}

TEST_F(ConcurrentRadixStoreTestFourThreads, InsertEraseSubtree) {
    initThreads(
        [](StringStore& tree, std::size_t term) {
            return tree.insert({"aaa", "a"}).second;
        },
        [](StringStore& tree, std::size_t term) {
            return tree.insert({"aaaa", "a"}).second;
        },
        [](StringStore& tree, std::size_t term) {
            return tree.insert({"aaab", "b"}).second;
        },
        [](StringStore& tree, std::size_t term) { return tree.erase("aaa"); });
    auto result = runThreads(stdx::chrono::seconds(3));
    if (auto res = result.find("aaa"); res != result.end()) {
        ASSERT_EQ(res->second, "a");
    }
    if (auto res = result.find("aaaa"); res != result.end()) {
        ASSERT_EQ(res->second, "a");
    }
    if (auto res = result.find("aaab"); res != result.end()) {
        ASSERT_EQ(res->second, "b");
    }
}

TEST_F(ConcurrentRadixStoreTestNineThreads, InsertEraseUpdateSameBranch) {
    AtomicWord<int> aTerm{0};
    AtomicWord<int> aaTerm{0};
    AtomicWord<int> aaaTerm{0};
    initThreads(
        [&](StringStore& tree, std::size_t term) {
            aTerm.store(term);
            return tree.insert({"a", std::string(term, 'a')}).second;
        },
        [&](StringStore& tree, std::size_t term) {
            aaTerm.store(term);
            return tree.insert({"aa", std::string(term, 'a')}).second;
        },
        [&](StringStore& tree, std::size_t term) {
            aaaTerm.store(term);
            return tree.insert({"aaa", std::string(term, 'a')}).second;
        },
        [&](StringStore& tree, std::size_t term) {
            aTerm.store(term);
            return tree.update({"a", std::string(term, 'a')}).second;
        },
        [&](StringStore& tree, std::size_t term) {
            aaTerm.store(term);
            return tree.update({"aa", std::string(term, 'a')}).second;
        },
        [&](StringStore& tree, std::size_t term) {
            aaaTerm.store(term);
            return tree.update({"aaa", std::string(term, 'a')}).second;
        },
        [](StringStore& tree, std::size_t term) { return tree.erase("a"); },
        [](StringStore& tree, std::size_t term) { return tree.erase("aa"); },
        [](StringStore& tree, std::size_t term) { return tree.erase("aaa"); });
    auto result = runThreads(stdx::chrono::seconds(3));
    if (auto res = result.find("a"); res != result.end()) {
        ASSERT_EQ(res->second, std::string(aTerm.load(), 'a'));
    }
    if (auto res = result.find("aa"); res != result.end()) {
        ASSERT_EQ(res->second, std::string(aaTerm.load(), 'a'));
    }
    if (auto res = result.find("aaa"); res != result.end()) {
        ASSERT_EQ(res->second, std::string(aaaTerm.load(), 'a'));
    }
}

}  // namespace biggie
}  // namespace mongo
