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
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/clock_source.h"

namespace mongo {
namespace {

class FTDCControllerTest : public FTDCTest {
public:
    FTDCControllerTest(uint64_t metadataCaptureFrequency = 1)
        : _metadataCaptureFrequency(metadataCaptureFrequency) {}
    void setMetadataCaptureFrequency(uint64_t metadataCaptureFrequency) {
        _metadataCaptureFrequency = metadataCaptureFrequency;
    }

protected:
    void testFull(UseMultiServiceSchema multiServiceSchema);
    void testStartAsDisabled(UseMultiServiceSchema multiServiceSchema);

private:
    uint64_t _metadataCaptureFrequency;
};

class FTDCMetricsCollectorMockTee : public FTDCCollectorInterface {
public:
    ~FTDCMetricsCollectorMockTee() override {
        ASSERT_TRUE(_state == State::kStarted);
    }

    void collect(OperationContext* opCtx, BSONObjBuilder& builder) final {
        _state = State::kStarted;

        stdx::unique_lock<Latch> lck(_mutex);

        ++_counter;

        // Generate document to return for collector
        generateDocument(builder, _counter);

        // Generate an entire document as if the FTDCCollector generates it
        {
            BSONObjBuilder b2;

            b2.appendDate(kFTDCCollectStartField,
                          getGlobalServiceContext()->getPreciseClockSource()->now());

            {
                BSONObjBuilder subObjBuilder(b2.subobjStart(name()));

                subObjBuilder.appendDate(kFTDCCollectStartField,
                                         getGlobalServiceContext()->getPreciseClockSource()->now());

                generateExpectedDocument(subObjBuilder, _counter);
                subObjBuilder.appendDate(kFTDCCollectEndField,
                                         getGlobalServiceContext()->getPreciseClockSource()->now());
            }

            b2.appendDate(kFTDCCollectEndField,
                          getGlobalServiceContext()->getPreciseClockSource()->now());

            _docs.emplace_back(b2.obj());
        }

        if (_counter == _wait) {
            _condvar.notify_all();
        }
    }

    std::string name() const final {
        return "mock";
    }

    virtual void generateDocument(BSONObjBuilder& builder, std::uint32_t counter) = 0;

    virtual void generateExpectedDocument(BSONObjBuilder& builder, std::uint32_t counter) {
        // Identical to generateDocument when the BSON is not compressed (e.g. for Periodic Metadata
        // in FTDCMetricsCollectorMockPeriodicMetadata)
        generateDocument(builder, counter);
    };

    void setSignalOnCount(int c) {
        _wait = c;
    }

    void wait() {
        stdx::unique_lock<Latch> lck(_mutex);
        while (_counter < _wait) {
            _condvar.wait(lck);
        }
    }

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

    std::uint32_t _counter{0};

    std::vector<BSONObj> _docs;

    Mutex _mutex = MONGO_MAKE_LATCH("FTDCMetricsCollectorMockTee::_mutex");
    stdx::condition_variable _condvar;
    std::uint32_t _wait{0};
};

class FTDCMetricsCollectorMock2 : public FTDCMetricsCollectorMockTee {
public:
    void generateDocument(BSONObjBuilder& builder, std::uint32_t counter) final {
        builder.append("name", "joe");
        builder.append("key1", static_cast<int32_t>(10 * counter + 1));
        builder.append("key2", static_cast<double>(counter * static_cast<int>(log10f(counter))));
    }
};

class FTDCMetricsCollectorMockPeriodicMetadata : public FTDCMetricsCollectorMockTee {
public:
    FTDCMetricsCollectorMockPeriodicMetadata(UseMultiServiceSchema multiServiceSchema) {
        _multiService = multiServiceSchema;
    };
    void generateDocument(BSONObjBuilder& builder, std::uint32_t counter) final {
        builder.append("name", "joeconfig");
        builder.append("key3", static_cast<int32_t>(10 * counter + 2));
        builder.append("key4", static_cast<double>(counter * static_cast<int>(log10f(counter))));
    }
    void generateExpectedDocument(BSONObjBuilder& builder, std::uint32_t counter) final {
        if (_multiService || !counter) {
            generateDocument(builder, counter);
            return;
        }
        std::string newName = "joeconfig";
        int32_t newKey3 = 10 * counter + 2;
        double newKey4 = counter * static_cast<int>(log10f(counter));
        if (newName != _nameCache) {
            _nameCache = newName;
            builder.append("name", _nameCache);
        };
        if (newKey3 != _key3Cache) {
            _key3Cache = newKey3;
            builder.append("key3", _key3Cache);
        };
        if (newKey4 != _key4Cache) {
            _key4Cache = newKey4;
            builder.append("key4", _key4Cache);
        };
    }

private:
    bool _multiService;
    std::string _nameCache = "-1";
    int32_t _key3Cache = -1;
    double _key4Cache = -1;
};

class FTDCMetricsCollectorMockRotate : public FTDCMetricsCollectorMockTee {
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

// Test a run of the controller and the data it logs to log file
void FTDCControllerTest::testFull(UseMultiServiceSchema multiServiceSchema) {
    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path dir(tempdir.path());

    createDirectoryClean(dir);

    FTDCConfig config;
    config.enabled = true;
    config.period = Milliseconds(1);
    config.metadataCaptureFrequency = _metadataCaptureFrequency;
    config.maxFileSizeBytes = FTDCConfig::kMaxFileSizeBytesDefault;
    config.maxDirectorySizeBytes = FTDCConfig::kMaxDirectorySizeBytesDefault;

    FTDCController c(dir, config, multiServiceSchema);

    auto c1 = std::make_unique<FTDCMetricsCollectorMock2>();
    auto c1Ptr = c1.get();
    c1Ptr->setSignalOnCount(100);

    auto c2 = std::make_unique<FTDCMetricsCollectorMockPeriodicMetadata>(multiServiceSchema);
    auto c2Ptr = c2.get();
    c2Ptr->setSignalOnCount(100 / _metadataCaptureFrequency);

    auto c3 = std::make_unique<FTDCMetricsCollectorMockRotate>();
    auto c3Ptr = c3.get();

    c.addPeriodicCollector(std::move(c1), ClusterRole::None);
    c.addPeriodicMetadataCollector(std::move(c2), ClusterRole::None);
    c.addOnRotateCollector(std::move(c3), ClusterRole::ShardServer);

    c.start(getClient()->getService());

    // Wait for 100 samples to have occured
    c1Ptr->wait();
    c2Ptr->wait();

    c.stop();

    auto docsPeriodic = c1Ptr->getDocs();
    ASSERT_GREATER_THAN_OR_EQUALS(docsPeriodic.size(), 100UL);
    auto docsPeriodicMetadata = c2Ptr->getDocs();
    ASSERT_GREATER_THAN_OR_EQUALS(docsPeriodicMetadata.size(), 100UL / _metadataCaptureFrequency);
    auto docsRotate = c3Ptr->getDocs();
    ASSERT_EQUALS(docsRotate.size(), 1UL);


    if (multiServiceSchema) {
        docsRotate = insertNewSchemaDocuments(docsRotate, "shard");
        docsPeriodicMetadata = insertNewSchemaDocuments(docsPeriodicMetadata, "common");
        docsPeriodic = insertNewSchemaDocuments(docsPeriodic, "common");
    }


    auto files = scanDirectory(dir);

    ASSERT_EQUALS(files.size(), 1UL);

    auto alog = files[0];

    std::vector<BSONObj> allDocs;
    allDocs.insert(allDocs.end(), docsRotate.cbegin(), docsRotate.cend());
    allDocs.insert(allDocs.end(), docsPeriodicMetadata.cbegin(), docsPeriodicMetadata.cend());
    allDocs.insert(allDocs.end(), docsPeriodic.cbegin(), docsPeriodic.cend());

    ValidateDocumentList(alog, allDocs, FTDCValidationMode::kStrict);
}

TEST_F(FTDCControllerTest, TestFullSingleServiceSchema) {
    setMetadataCaptureFrequency(1);
    testFull(UseMultiServiceSchema{false});

    setMetadataCaptureFrequency(3);
    testFull(UseMultiServiceSchema{false});
}

TEST_F(FTDCControllerTest, TestFullMultiserviceSchema) {
    setMetadataCaptureFrequency(1);
    testFull(UseMultiServiceSchema{true});

    setMetadataCaptureFrequency(3);
    testFull(UseMultiServiceSchema{true});
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

// Test we can start the controller as disabled, the directory is empty, and then we can succesfully
// enable it
void FTDCControllerTest::testStartAsDisabled(UseMultiServiceSchema multiServiceSchema) {
    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path dir(tempdir.path());

    createDirectoryClean(dir);

    FTDCConfig config;
    config.enabled = false;
    config.period = Milliseconds(1);
    config.metadataCaptureFrequency = _metadataCaptureFrequency;
    config.maxFileSizeBytes = FTDCConfig::kMaxFileSizeBytesDefault;
    config.maxDirectorySizeBytes = FTDCConfig::kMaxDirectorySizeBytesDefault;

    auto c1 = std::make_unique<FTDCMetricsCollectorMock2>();
    auto c2 = std::make_unique<FTDCMetricsCollectorMockPeriodicMetadata>(multiServiceSchema);

    auto c1Ptr = c1.get();
    auto c2Ptr = c2.get();

    FTDCController c(dir, config, multiServiceSchema);

    c.addPeriodicCollector(std::move(c1), ClusterRole::ShardServer);
    c.addPeriodicMetadataCollector(std::move(c2), ClusterRole::ShardServer);

    c.start(getClient()->getService());

    auto files0 = scanDirectory(dir);

    ASSERT_EQUALS(files0.size(), 0UL);

    ASSERT_OK(c.setEnabled(true));

    c1Ptr->setSignalOnCount(50);
    c2Ptr->setSignalOnCount(50 / _metadataCaptureFrequency);

    // Wait for 50 samples to have occured
    c1Ptr->wait();
    c2Ptr->wait();

    c.stop();

    auto docsPeriodic = c1Ptr->getDocs();
    ASSERT_GREATER_THAN_OR_EQUALS(docsPeriodic.size(), 50UL);

    auto docsPeriodicMetadata = c2Ptr->getDocs();
    ASSERT_GREATER_THAN_OR_EQUALS(docsPeriodicMetadata.size(), 50UL / _metadataCaptureFrequency);

    if (multiServiceSchema) {
        docsPeriodic = insertNewSchemaDocuments(docsPeriodic, "shard");
        docsPeriodicMetadata = insertNewSchemaDocuments(docsPeriodicMetadata, "shard");
    }

    std::vector<BSONObj> allDocs;
    allDocs.insert(allDocs.end(), docsPeriodicMetadata.cbegin(), docsPeriodicMetadata.cend());
    allDocs.insert(allDocs.end(), docsPeriodic.cbegin(), docsPeriodic.cend());

    auto files = scanDirectory(dir);

    ASSERT_EQUALS(files.size(), 1UL);

    auto alog = files[0];

    ValidateDocumentList(alog, allDocs, FTDCValidationMode::kStrict);
}

TEST_F(FTDCControllerTest, TestStartAsDisabledSingleServiceSchema) {
    setMetadataCaptureFrequency(1);
    testStartAsDisabled(UseMultiServiceSchema{false});

    setMetadataCaptureFrequency(3);
    testStartAsDisabled(UseMultiServiceSchema{false});
}

TEST_F(FTDCControllerTest, TestStartAsDisabledMultiserviceSchema) {
    setMetadataCaptureFrequency(1);
    testStartAsDisabled(UseMultiServiceSchema{true});

    setMetadataCaptureFrequency(3);
    testStartAsDisabled(UseMultiServiceSchema{true});
}

}  // namespace
}  // namespace mongo
