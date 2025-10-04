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
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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
    stdx::mutex _mutex;
    stdx::condition_variable _cv;
    bool _released = false;
    uint64_t _progress = 0;
    uint64_t _limit = 1;
};

class MockFailCollector : public FTDCCollectorInterface {
public:
    void collect(OperationContext*, BSONObjBuilder&) final {
        throw std::logic_error("MockFailController");
    }

    std::string name() const final {
        return "MockFailCollector";
    }
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
    void setUpControllerAndCheckpoint(FTDCConfig config) {
        createDirectoryClean(_dir);

        auto env = std::make_unique<MockControllerEnv>();
        _checkpoint = &env->loopCheckpoint;

        _controller = std::make_unique<FTDCController>(_dir, config, std::move(env));
    }

    void startController() {
        _controller->start(getClient()->getService());
        _checkpoint->wait();
    }

    void releaseCheckpointAndStopController() {
        _checkpoint->release();
        _controller->stop();
    }

    const boost::filesystem::path& dir() const {
        return _dir;
    }

    void doCollection() {
        _checkpoint->advance();
        _checkpoint->wait();
    }

    void testPeriodicCollector(bool enabled, std::unique_ptr<MockCollector> collector);
    void testRotateCollector(int numRotations, std::unique_ptr<MockCollector> collector);

    FTDCController* controller() {
        return _controller.get();
    }

private:
    uint64_t _metadataCaptureFrequency;
    unittest::TempDir _tempdir{"metrics_testpath"};
    boost::filesystem::path _dir{_tempdir.path()};
    Checkpoint* _checkpoint;
    std::unique_ptr<FTDCController> _controller;
};

auto toMetadataCollector(MockCollector* collector) {
    return dynamic_cast<MockPeriodicMetadataCollector*>(collector);
}

// Test a run of the controller with a single periodicCollector and the data it logs to log file
void FTDCControllerTest::testPeriodicCollector(bool enabled,
                                               std::unique_ptr<MockCollector> collector) {
    FTDCConfig config;
    config.enabled = enabled;
    config.period = Milliseconds(1);
    config.metadataCaptureFrequency = _metadataCaptureFrequency;
    config.maxFileSizeBytes = FTDCConfig::kMaxFileSizeBytesDefault;
    config.maxDirectorySizeBytes = FTDCConfig::kMaxDirectorySizeBytesDefault;

    setUpControllerAndCheckpoint(config);

    uint64_t numCollections = 3;

    auto collectorPtr = collector.get();
    if (toMetadataCollector(collectorPtr)) {
        _controller->addPeriodicMetadataCollector(std::move(collector));
    } else {
        _controller->addPeriodicCollector(std::move(collector));
    }

    _controller->start(getClient()->getService());
    if (!enabled) {
        auto files = scanDirectory(_dir);
        ASSERT_EQUALS(files.size(), 0);
        ASSERT_OK(_controller->setEnabled(true));
    }
    _checkpoint->wait();

    // Wait for numCollections samples to have occured
    LOGV2_DEBUG(9129201, 0, "Collecting");
    auto collectUntilDocCount = [&](auto& collectorPtr, size_t docs) {
        while (collectorPtr->getDocs().size() < docs)
            doCollection();
    };
    collectUntilDocCount(collectorPtr, numCollections);

    releaseCheckpointAndStopController();

    auto docs = collectorPtr->getDocs();
    ASSERT_GTE(docs.size(),
               toMetadataCollector(collectorPtr) ? numCollections
                                                 : numCollections / _metadataCaptureFrequency);

    auto files = scanDirectory(_dir);
    ASSERT_EQUALS(files.size(), 1);

    decltype(docs) metaDocs;
    if (toMetadataCollector(collectorPtr))
        std::swap(metaDocs, docs);
    ValidateDocumentListByType(files, {}, docs, metaDocs, FTDCValidationMode::kStrict);
}

void FTDCControllerTest::testRotateCollector(int numRotations,
                                             std::unique_ptr<MockCollector> collector) {
    FTDCConfig config;
    config.period = Milliseconds(100);
    setUpControllerAndCheckpoint(config);

    auto collectorPtr = collector.get();
    _controller->addOnRotateCollector(std::move(collector));

    startController();
    for (int i = numRotations; i--;) {
        _controller->triggerRotate();
        doCollection();
    }
    releaseCheckpointAndStopController();

    // A rotation closes the current file and creates a new one so if we rotate n times we have 1 +
    // n total files.
    auto expectedNumFiles = 1 + numRotations;

    auto docs = collectorPtr->getDocs();
    ASSERT_EQUALS(docs.size(), expectedNumFiles);

    auto files = scanDirectory(_dir);
    ASSERT_EQUALS(files.size(), expectedNumFiles);

    ValidateDocumentListByType(files, docs, {}, {}, FTDCValidationMode::kStrict);
}

TEST_F(FTDCControllerTest, TestPeriodicStartingEnabled) {
    testPeriodicCollector(true, std::make_unique<MockPeriodicCollector>());
}

TEST_F(FTDCControllerTest, TestMetadataStartingEnabled) {
    setMetadataCaptureFrequency(3);
    testPeriodicCollector(true, std::make_unique<MockPeriodicMetadataCollector>());
}

TEST_F(FTDCControllerTest, TestPeriodicStartingDisabled) {
    testPeriodicCollector(false, std::make_unique<MockPeriodicCollector>());
}

TEST_F(FTDCControllerTest, TestMetadataStartingDisabled) {
    setMetadataCaptureFrequency(3);
    testPeriodicCollector(false, std::make_unique<MockPeriodicMetadataCollector>());
}

TEST_F(FTDCControllerTest, TestRotate1) {
    testRotateCollector(1, std::make_unique<MockRotateCollector>());
}
TEST_F(FTDCControllerTest, TestRotate20) {
    testRotateCollector(20, std::make_unique<MockRotateCollector>());
}

// Test we can start and stop the controller in quick succession, make sure it succeeds without
// assert or fault
TEST_F(FTDCControllerTest, TestStartStop) {
    FTDCConfig config;
    config.enabled = false;
    config.period = Milliseconds(1);
    config.metadataCaptureFrequency = 1;

    setUpControllerAndCheckpoint(config);

    startController();
    releaseCheckpointAndStopController();
}

DEATH_TEST_REGEX_F(FTDCControllerTest,
                   LogAndTerminateWhenCollectionFails,
                   "Fatal assertion.*9399800") {
    FTDCConfig config;
    config.period = Milliseconds(100);
    setUpControllerAndCheckpoint(config);

    // Remove RW permissions from the directory to force the FTDC thread to throw.
    boost::filesystem::permissions(dir(), boost::filesystem::no_perms);

    startController();

    // Do a single sample collection to ensure we run through FTDCController::doLoop() and die.
    doCollection();
}

DEATH_TEST_REGEX_F(FTDCControllerTest,
                   LogAndTerminateWhenExceptionThrown,
                   "9761500.*MockFailCollector") {
    FTDCConfig config;
    config.period = Milliseconds(100);
    setUpControllerAndCheckpoint(config);

    auto collector = std::make_unique<MockFailCollector>();
    controller()->addPeriodicCollector(std::move(collector));

    startController();

    // Do a single sample collection to ensure we run through FTDCController::doLoop() and die.
    doCollection();
}

}  // namespace
}  // namespace mongo
