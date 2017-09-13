/**
 *    Copyright (C) 2017 10gen Inc.
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

#include "mongo/platform/basic.h"

#include <chrono>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/client.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/deferred_writer.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/stdx/chrono.h"

namespace deferred_writer_tests {

namespace {
AtomicWord<int64_t> counter;

// Get a new well-formed object with a unique _id field.
BSONObj getObj(void) {
    return BSON("_id" << counter.fetchAndAdd(1));
}

// An STL-style Compare for BSONObjs.
struct BSONObjCompare {
    bool operator()(const BSONObj& lhs, const BSONObj& rhs) const {
        return SimpleBSONObjComparator::kInstance.compare(lhs, rhs) < 0;
    }
};
}

static const NamespaceString kTestNamespace("unittests", "deferred_writer_tests");

/**
 * For exception-safe code with DeferredWriter.
 *
 * If a test fails with a DeferredWriter wrapped in one of these it doesn't crash the server.  Note
 * that this is not in general a good idea, because it has a potentially blocking destructor.
 */
class RaiiWrapper {
public:
    explicit RaiiWrapper(std::unique_ptr<DeferredWriter> writer) : _writer(std::move(writer)) {
        _writer->startup("DeferredWriter test");
    }

    RaiiWrapper(RaiiWrapper&& other) : _writer(std::move(other._writer)) {}

    ~RaiiWrapper() {
        _writer->shutdown();
    }

    DeferredWriter* get(void) {
        return _writer.get();
    }

private:
    std::unique_ptr<DeferredWriter> _writer;
};

/**
 * Provides some handy utilities.
 */
class DeferredWriterTestBase {
public:
    DeferredWriterTestBase() : _client(_opCtx.get()) {}

    virtual ~DeferredWriterTestBase() {}

    void createCollection(void) {
        _client.createCollection(kTestNamespace.toString());
    }

    void dropCollection(void) {
        if (AutoGetCollection(_opCtx.get(), kTestNamespace, MODE_IS).getCollection()) {
            _client.dropCollection(kTestNamespace.toString());
        }
    }

    void ensureEmpty(void) {
        dropCollection();
        createCollection();
    }

    /**
     * Just read the whole collection into memory.
     */
    std::vector<BSONObj> readCollection(void) {
        AutoGetCollection agc(_opCtx.get(), kTestNamespace, MODE_IS);
        ASSERT_TRUE(agc.getCollection());

        auto plan = InternalPlanner::collectionScan(
            _opCtx.get(), kTestNamespace.ns(), agc.getCollection(), PlanExecutor::NO_YIELD);

        std::vector<BSONObj> result;
        BSONObj i;
        while (plan->getNext(&i, nullptr) == PlanExecutor::ExecState::ADVANCED) {
            result.push_back(i);
        }

        return result;
    }

    /**
     * Get a writer to the test collection.
     */
    RaiiWrapper getWriter(CollectionOptions options = CollectionOptions(),
                          int64_t maxSize = 200'000) {
        return RaiiWrapper(stdx::make_unique<DeferredWriter>(kTestNamespace, options, maxSize));
    }

    virtual void run(void) = 0;

protected:
    const ServiceContext::UniqueOperationContext _opCtx = cc().makeOperationContext();
    DBDirectClient _client;
};

/**
 * Launch a bunch of threads and wait until they all finish.
 */
class ThreadLauncher {
public:
    template <typename T, typename... Args>
    void launch(int count, T&& task, Args&&... args) {
        for (int i = 0; i < count; ++i) {
            threads.push_back(stdx::thread(task, args...));
        }
    }

    void await(void) {
        for (auto& thread : threads) {
            thread.join();
        }
    }

private:
    std::vector<stdx::thread> threads;
};

/**
 * Test that the deferred writer will create the collection if it is empty.
 */
class DeferredWriterTestEmpty : public DeferredWriterTestBase {
public:
    ~DeferredWriterTestEmpty(){};

    void run() {
        {
            auto gw = getWriter();
            auto writer = gw.get();
            writer->insertDocument(getObj());
        }
        ASSERT_TRUE(AutoGetCollection(_opCtx.get(), kTestNamespace, MODE_IS).getCollection());
        ASSERT_TRUE(readCollection().size() == 1);
    }
};

/**
 * Test that concurrent inserts to the DeferredWriter work, and never drop writes.
 */
class DeferredWriterTestConcurrent : public DeferredWriterTestBase {
public:
    ~DeferredWriterTestConcurrent(){};

    void worker(DeferredWriter* writer) {
        for (int i = 0; i < kDocsPerWorker; ++i) {
            writer->insertDocument(getObj());
        }
    }

    void run() {
        ensureEmpty();
        {
            auto gw = getWriter();
            auto writer = gw.get();
            ThreadLauncher launcher;
            // Launch some threads inserting into the writer.
            launcher.launch(kNWorkers, &DeferredWriterTestConcurrent::worker, this, writer);
            launcher.await();
        }
        ASSERT_EQ(readCollection().size(), (size_t)(kNWorkers * kDocsPerWorker));
    }

private:
    static const int kNWorkers = 20;
    static const int kDocsPerWorker = 100;
};

bool compareBsonObjects(const BSONObj& lhs, const BSONObj& rhs) {
    return SimpleBSONObjComparator::kInstance.compare(lhs, rhs) < 0;
}

/**
 * Test that the documents make it through the writer unchanged.
 */
class DeferredWriterTestConsistent : public DeferredWriterTestBase {
public:
    ~DeferredWriterTestConsistent() {}

    void run() {
        ensureEmpty();
        {
            auto gw = getWriter();
            auto writer = gw.get();
            for (int i = 0; i < 1000; ++i) {
                auto obj = getObj();
                _added.insert(obj);
                writer->insertDocument(obj);
            }
        }

        auto contents = readCollection();
        BSONSet found(contents.begin(), contents.end());

        // Check set equality between found and _added.
        auto i1 = found.begin();
        for (auto i2 = _added.begin(); i2 != _added.end(); ++i2) {
            ASSERT_TRUE(i1 != found.end());
            ASSERT_EQ(SimpleBSONObjComparator::kInstance.compare(*i1, *i2), 0);
            ++i1;
        }
        ASSERT_TRUE(i1 == found.end());
    }

private:
    using BSONSet = std::set<BSONObj, BSONObjCompare>;
    BSONSet _added;
};

/**
 * Test that the writer works when a global X lock is held by the caller.
 */
class DeferredWriterTestNoDeadlock : public DeferredWriterTestBase {
public:
    void run(void) {
        int nDocs = 1000;
        ensureEmpty();
        {
            auto gw = getWriter();
            auto writer = gw.get();
            Lock::GlobalWrite lock(_opCtx.get());

            for (int i = 0; i < nDocs; ++i) {
                writer->insertDocument(getObj());
            }

            // Make sure it hasn't added to the collection under our X lock.
            ASSERT_EQ((size_t)0, readCollection().size());
        }
        ASSERT_EQ((size_t)nDocs, readCollection().size());
    }
};

/**
 * Test that the DeferredWriter rejects documents over the buffer size.
 * When this happens, check that the logging counter resets after the first
 * write.
 * Note: This test assumes that the logging interval is sufficiently large
 * and that the first dropped write is the ONLY one logged.
 */
class DeferredWriterTestCap : public DeferredWriterTestBase {
public:
    void run(void) {
        // Add a few hundred documents.
        int maxDocs = 500;
        // (more than can fit in a 2KB buffer).
        int bufferSize = 2'000;

        // Keep track of what we add.
        int bytesAdded = 0;
        int nAdded = 0;
        int nDropped = 0;

        ensureEmpty();
        {
            auto gw = getWriter(CollectionOptions(), bufferSize);
            auto writer = gw.get();

            // Start with 0 dropped entries
            ASSERT_EQ(0, writer->getDroppedEntries());

            // Don't let it flush the buffer while we're working.
            Lock::GlobalWrite lock(_opCtx.get());
            for (int i = 0; i < maxDocs; ++i) {
                auto obj = getObj();

                if (bytesAdded + obj.objsize() > bufferSize) {
                    // Should return false when we exceed the buffer size.
                    ASSERT(!writer->insertDocument(obj));
                    ++nDropped;
                } else {
                    ASSERT(writer->insertDocument(obj));
                    bytesAdded += obj.objsize();
                    ++nAdded;
                }
                // Check that the first dropped write (assuming a long
                // interval) resets the internal counter by 1.
                if (nDropped >= 1) {
                    ASSERT_EQ(nDropped, 1 + writer->getDroppedEntries());
                }
            }
        }
        // Make sure it didn't add any of the rejected documents.
        ASSERT_EQ(readCollection().size(), static_cast<size_t>(nAdded));
    }
};

/**
 * Test that the inserts are sometimes actually executed without flushing.
 */
class DeferredWriterTestAsync : public DeferredWriterTestBase {
public:
    void worker(DeferredWriter* writer) {
        for (int i = 0; i < kDocsPerWorker; ++i) {
            writer->insertDocument(getObj());
        }
    }

    void run(void) {
        using namespace std::chrono_literals;
        ensureEmpty();
        ThreadLauncher launcher;
        auto gw = getWriter();
        auto writer = gw.get();
        launcher.launch(kNWorkers, &DeferredWriterTestAsync::worker, this, writer);
        launcher.await();

        auto start = stdx::chrono::system_clock::now();

        // Spin-wait for one minute or until something has been added to the collection.
        while (stdx::chrono::system_clock::now() - start < 1min && readCollection().size() == 0) {
            stdx::this_thread::yield();
        }

        // Buffer should have flushed by now.
        ASSERT_GT(readCollection().size(), (size_t)0);
    }

private:
    static const int kNWorkers = 20;
    static const int kDocsPerWorker = 100;
};

class DeferredWriterTests : public Suite {
public:
    DeferredWriterTests() : Suite("deferred_writer_tests") {}

    void setupTests() {
        add<DeferredWriterTestEmpty>();
        add<DeferredWriterTestConcurrent>();
        add<DeferredWriterTestConsistent>();
        add<DeferredWriterTestNoDeadlock>();
        add<DeferredWriterTestCap>();
        add<DeferredWriterTestAsync>();
    }
} deferredWriterTests;
}
