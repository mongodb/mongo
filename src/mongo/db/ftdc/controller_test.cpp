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

#include <boost/filesystem/path.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/constants.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/ftdc/ftdc_test.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/clock_source.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class Checkpoint {
public:
    void arrive() {
        stdx::unique_lock lk{_mutex};
        if (_released)
            return;

        ++_progress;
        _cv.notify_all();
        _cv.wait(lk, [&] { return _progress < _limit; });
    }

    void wait() {
        stdx::unique_lock lk{_mutex};
        _cv.wait(lk, [&] { return _progress == _limit; });
    }

    void advance() {
        stdx::unique_lock lk{_mutex};
        ++_limit;
        _cv.notify_all();
    }

    void release() {
        stdx::unique_lock lk{_mutex};
        _released = true;
        _progress = 0;
        _cv.notify_all();
    }

private:
    stdx::mutex _mutex;  // NOLINT
    stdx::condition_variable _cv;
    bool _released = false;
    uint64_t _progress = 0;
    uint64_t _limit = 1;
};

class MockCollector : public FTDCCollectorInterface {
public:
    void collect(OperationContext* opCtx, BSONObjBuilder& builder) final {
        _state = State::kStarted;

        ++_collectorWrites;

        // Generate document to return for collector
        generateDocument(builder, _collectorWrites);

        // Generate an entire document as if the FTDCCollector generates it
        {
            BSONObjBuilder b2;

            b2.appendDate(kFTDCCollectStartField,
                          getGlobalServiceContext()->getPreciseClockSource()->now());

            {
                BSONObjBuilder subObjBuilder(b2.subobjStart(name()));

                subObjBuilder.appendDate(kFTDCCollectStartField,
                                         getGlobalServiceContext()->getPreciseClockSource()->now());

                generateExpectedDocument(subObjBuilder, _collectorWrites);
                subObjBuilder.appendDate(kFTDCCollectEndField,
                                         getGlobalServiceContext()->getPreciseClockSource()->now());
            }

            b2.appendDate(kFTDCCollectEndField,
                          getGlobalServiceContext()->getPreciseClockSource()->now());

            _docs.emplace_back(b2.obj());
        }
    }

    std::string name() const final {
        return "mock";
    }

    virtual void generateDocument(BSONObjBuilder& builder, std::uint32_t counter) = 0;

    virtual void generateExpectedDocument(BSONObjBuilder& builder, std::uint32_t counter) {
        // Identical to generateDocument when the BSON is not compressed (e.g. for Periodic Metadata
        // in MockPeriodicMetadataCollector)
        generateDocument(builder, counter);
    };

    std::vector<BSONObj>& getDocs() {
        return _docs;
    }

private:
    /**
     * Private enum to ensure caller uses class correctly.
     */
    enum class State {
        kNotStarted,
        kStarted,
    };

    // state
    State _state{State::kNotStarted};

    std::uint32_t _collectorWrites = 0;

    std::vector<BSONObj> _docs;
};

class MockPeriodicCollector : public MockCollector {
public:
    void generateDocument(BSONObjBuilder& builder, std::uint32_t counter) final {
        builder.append("name", "joe");
        builder.append("key1", static_cast<int32_t>(10 * counter + 1));
        builder.append("key2", static_cast<double>(counter * static_cast<int>(log10f(counter))));
    }
};

class MockPeriodicMetadataCollector : public MockCollector {
public:
    explicit MockPeriodicMetadataCollector(UseMultiServiceSchema multiServiceSchema)
        : _multiService(std::move(multiServiceSchema)) {}

    void generateDocument(BSONObjBuilder& builder, std::uint32_t counter) final {
        builder.append("name", "joeconfig");
        builder.append("key3", static_cast<int32_t>(10 * counter + 2));
        builder.append("key4", static_cast<double>(counter * static_cast<int>(log10f(counter))));
    }
    void generateExpectedDocument(BSONObjBuilder& builder, std::uint32_t counter) final {
        std::string newName = "joeconfig";
        int32_t newKey3 = 10 * counter + 2;
        double newKey4 = counter * static_cast<int>(log10f(counter));

        if (newName != _nameCache) {
            _nameCache = newName;
            builder.append("name", _nameCache);
        }
        if (newKey3 != _key3Cache) {
            _key3Cache = newKey3;
            builder.append("key3", _key3Cache);
        }
        if (newKey4 != _key4Cache) {
            _key4Cache = newKey4;
            builder.append("key4", _key4Cache);
        }
    }

private:
    bool _multiService;
    boost::filesystem::path _dir;
    size_t _numFiles = 1;

    // Cache is cleared when a value changes or when a rotate occurs.
    std::string _nameCache = "-1";
    int32_t _key3Cache = -1;
    double _key4Cache = -1;
};

class MockRotateCollector : public MockCollector {
public:
    void generateDocument(BSONObjBuilder& builder, std::uint32_t counter) final {
        builder.append("name", "joerotate");
        builder.append("hostinfo", static_cast<int32_t>(10 * counter + 3));
        builder.append("buildinfo", 53);
    }
};

std::vector<BSONObj> insertNewSchemaDocuments(const std::vector<BSONObj>& docs, StringData role) {
    std::vector<BSONObj> newDocs;
    for (const auto& doc : docs) {
        constexpr static auto dummyTs = Date_t::fromMillisSinceEpoch(1);
        newDocs.push_back(BSONObjBuilder{}
                              .append("start", dummyTs)
                              .append(role, doc)
                              .append("end", dummyTs)
                              .obj());
    }
    return newDocs;
}

/**
 * Used to sync the flow of the FTDCController with its test. FTDCController calls onStartLoop() at
 * the start of each collection loop and it will block until the test calls
 * loopCheckpoint.advance(). Calling loopCheckpoint.wait() after a loopCheckpoint.advance() will
 * block the test until the controller finishes its loop body. A call to loopCheckpoint.release()
 * will make the FTDCController run async.
 */
class MockControllerEnv : public FTDCController::Env {
public:
    void onStartLoop() override {
        loopCheckpoint.arrive();
    }

    Checkpoint loopCheckpoint;
};

class FTDCControllerTest : public FTDCTest {
public:
    explicit FTDCControllerTest(uint64_t metadataCaptureFrequency = 1)
        : _metadataCaptureFrequency(metadataCaptureFrequency) {}

    void setMetadataCaptureFrequency(uint64_t metadataCaptureFrequency) {
        _metadataCaptureFrequency = metadataCaptureFrequency;
    }

protected:
    void testPeriodicCollector(UseMultiServiceSchema multiServiceSchema,
                               bool enabled,
                               std::unique_ptr<MockCollector> collector);
    void testRotateCollector(UseMultiServiceSchema multiServiceSchema,
                             int numRotations,
                             std::unique_ptr<MockCollector> collector);

private:
    uint64_t _metadataCaptureFrequency;
};

auto toMetadataCollector(MockCollector* collector) {
    return dynamic_cast<MockPeriodicMetadataCollector*>(collector);
}

// Test a run of the controller with a single periodicCollector and the data it logs to log file
void FTDCControllerTest::testPeriodicCollector(UseMultiServiceSchema multiServiceSchema,
                                               bool enabled,
                                               std::unique_ptr<MockCollector> collector) {
    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path dir(tempdir.path());

    createDirectoryClean(dir);

    FTDCConfig config;
    config.enabled = enabled;
    config.period = Milliseconds(1);
    config.metadataCaptureFrequency = _metadataCaptureFrequency;
    config.maxFileSizeBytes = FTDCConfig::kMaxFileSizeBytesDefault;
    config.maxDirectorySizeBytes = FTDCConfig::kMaxDirectorySizeBytesDefault;

    auto env = std::make_unique<MockControllerEnv>();
    Checkpoint& checkpoint = env->loopCheckpoint;

    FTDCController c(dir, config, multiServiceSchema, std::move(env));

    uint64_t numCollections = 3;

    auto collectorPtr = collector.get();
    if (toMetadataCollector(collectorPtr)) {
        c.addPeriodicMetadataCollector(std::move(collector), ClusterRole::None);
    } else {
        c.addPeriodicCollector(std::move(collector), ClusterRole::None);
    }

    c.start(getClient()->getService());
    if (!enabled) {
        auto files = scanDirectory(dir);
        ASSERT_EQUALS(files.size(), 0);
        ASSERT_OK(c.setEnabled(true));
    }

    checkpoint.wait();

    auto doCollection = [&](uint64_t n = 1) {
        while (n--) {
            checkpoint.advance();
            checkpoint.wait();
        }
    };

    // Wait for numCollections samples to have occured
    LOGV2_DEBUG(9129201, 0, "Collecting");

    auto collectUntilDocCount = [&](auto& collectorPtr, size_t docs) {
        while (collectorPtr->getDocs().size() < docs)
            doCollection(1);
    };

    collectUntilDocCount(collectorPtr, numCollections);
    checkpoint.release();
    c.stop();

    auto docs = collectorPtr->getDocs();
    ASSERT_GTE(docs.size(),
               toMetadataCollector(collectorPtr) ? numCollections
                                                 : numCollections / _metadataCaptureFrequency);

    if (multiServiceSchema) {
        docs = insertNewSchemaDocuments(docs, "common");
    }

    auto files = scanDirectory(dir);
    ASSERT_EQUALS(files.size(), 1);

    decltype(docs) metaDocs;
    if (toMetadataCollector(collectorPtr))
        std::swap(metaDocs, docs);
    ValidateDocumentListByType(files, {}, docs, metaDocs, FTDCValidationMode::kStrict);
}

void FTDCControllerTest::testRotateCollector(UseMultiServiceSchema multiServiceSchema,
                                             int numRotations,
                                             std::unique_ptr<MockCollector> collector) {
    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path dir(tempdir.path());

    createDirectoryClean(dir);

    auto env = std::make_unique<MockControllerEnv>();
    Checkpoint& checkpoint = env->loopCheckpoint;

    auto collectorPtr = collector.get();

    FTDCConfig config;
    config.period = Milliseconds(100);

    FTDCController c(dir, config, multiServiceSchema, std::move(env));
    c.addOnRotateCollector(std::move(collector), ClusterRole::ShardServer);
    c.start(getClient()->getService());
    checkpoint.wait();

    for (int i = numRotations; i--;) {
        c.triggerRotate();
        checkpoint.advance();
        checkpoint.wait();
    }
    checkpoint.release();
    c.stop();

    // A rotation closes the current file and creates a new one so if we rotate n times we have 1 +
    // n total files.
    auto expectedNumFiles = 1 + numRotations;

    auto docs = collectorPtr->getDocs();
    ASSERT_EQUALS(docs.size(), expectedNumFiles);

    if (multiServiceSchema) {
        docs = insertNewSchemaDocuments(docs, "shard");
    }

    auto files = scanDirectory(dir);
    ASSERT_EQUALS(files.size(), expectedNumFiles);

    ValidateDocumentListByType(files, docs, {}, {}, FTDCValidationMode::kStrict);
}

TEST_F(FTDCControllerTest, TestPeriodicStartingEnabledWithMultiversion) {
    testPeriodicCollector(
        UseMultiServiceSchema{true}, true, std::make_unique<MockPeriodicCollector>());
}

TEST_F(FTDCControllerTest, TestMetadataStartingEnabledWithMultiversion) {
    setMetadataCaptureFrequency(3);
    UseMultiServiceSchema multiServiceSchema{true};
    testPeriodicCollector(multiServiceSchema,
                          true,
                          std::make_unique<MockPeriodicMetadataCollector>(multiServiceSchema));
}

TEST_F(FTDCControllerTest, TestPeriodicStartingEnabledWithoutMultiversion) {
    testPeriodicCollector(
        UseMultiServiceSchema{false}, true, std::make_unique<MockPeriodicCollector>());
}

TEST_F(FTDCControllerTest, TestMetadataStartingEnabledWithoutMultiversion) {
    setMetadataCaptureFrequency(3);
    UseMultiServiceSchema multiServiceSchema{false};
    testPeriodicCollector(multiServiceSchema,
                          true,
                          std::make_unique<MockPeriodicMetadataCollector>(multiServiceSchema));
}

TEST_F(FTDCControllerTest, TestPeriodicStartingDisabledWithMultiversion) {
    testPeriodicCollector(
        UseMultiServiceSchema{true}, false, std::make_unique<MockPeriodicCollector>());
}

TEST_F(FTDCControllerTest, TestMetadataStartingDisabledWithMultiversion) {
    setMetadataCaptureFrequency(3);
    UseMultiServiceSchema multiServiceSchema{true};
    testPeriodicCollector(multiServiceSchema,
                          false,
                          std::make_unique<MockPeriodicMetadataCollector>(multiServiceSchema));
}

TEST_F(FTDCControllerTest, TestPeriodicStartingDisabledWithoutMultiversion) {
    testPeriodicCollector(
        UseMultiServiceSchema{false}, false, std::make_unique<MockPeriodicCollector>());
}

TEST_F(FTDCControllerTest, TestMetadataStartingDisabledWithoutMultiversion) {
    setMetadataCaptureFrequency(3);
    UseMultiServiceSchema multiServiceSchema{false};
    testPeriodicCollector(multiServiceSchema,
                          false,
                          std::make_unique<MockPeriodicMetadataCollector>(multiServiceSchema));
}

TEST_F(FTDCControllerTest, TestRotate1WithMultiversion) {
    testRotateCollector(UseMultiServiceSchema{true}, 1, std::make_unique<MockRotateCollector>());
}
TEST_F(FTDCControllerTest, TestRotate20WithMultiversion) {
    testRotateCollector(UseMultiServiceSchema{true}, 20, std::make_unique<MockRotateCollector>());
}

TEST_F(FTDCControllerTest, TestRotate1WithoutMultiversion) {
    testRotateCollector(UseMultiServiceSchema{false}, 1, std::make_unique<MockRotateCollector>());
}
TEST_F(FTDCControllerTest, TestRotate20WithoutMultiversion) {
    testRotateCollector(UseMultiServiceSchema{false}, 20, std::make_unique<MockRotateCollector>());
}

// Test we can start and stop the controller in quick succession, make sure it succeeds without
// assert or fault
TEST_F(FTDCControllerTest, TestStartStop) {
    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path dir(tempdir.path());

    createDirectoryClean(dir);

    FTDCConfig config;
    config.enabled = false;
    config.period = Milliseconds(1);
    config.metadataCaptureFrequency = 1;
    config.maxFileSizeBytes = FTDCConfig::kMaxFileSizeBytesDefault;
    config.maxDirectorySizeBytes = FTDCConfig::kMaxDirectorySizeBytesDefault;

    FTDCController c(dir, config, UseMultiServiceSchema{true});

    c.start(getClient()->getService());

    c.stop();
}

}  // namespace
}  // namespace mongo
