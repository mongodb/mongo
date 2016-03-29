/**
 * Copyright (C) 2015 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/filesystem.hpp>
#include <iostream>

#include "mongo/base/data_type_validated.h"
#include "mongo/base/init.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/ftdc/collector.h"
#include "mongo/db/ftdc/config.h"
#include "mongo/db/ftdc/constants.h"
#include "mongo/db/ftdc/controller.h"
#include "mongo/db/ftdc/ftdc_test.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class FTDCMetricsCollectorMockTee : public FTDCCollectorInterface {
public:
    ~FTDCMetricsCollectorMockTee() {
        ASSERT_TRUE(_state == State::kStarted);
    }

    void collect(OperationContext* txn, BSONObjBuilder& builder) final {
        _state = State::kStarted;
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

                generateDocument(subObjBuilder, _counter);

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

    void setSignalOnCount(int c) {
        _wait = c;
    }

    void wait() {
        stdx::unique_lock<stdx::mutex> lck(_mutex);
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

    stdx::mutex _mutex;
    stdx::condition_variable _condvar;
    std::uint32_t _wait{0};
};

class FTDCMetricsCollectorMock2 : public FTDCMetricsCollectorMockTee {
public:
    void generateDocument(BSONObjBuilder& builder, std::uint32_t counter) final {
        builder.append("name", "joe");
        builder.append("key1", (counter * 37));
        builder.append("key2", static_cast<double>(counter * static_cast<int>(log10f(counter))));
    }
};

class FTDCMetricsCollectorMockRotate : public FTDCMetricsCollectorMockTee {
public:
    void generateDocument(BSONObjBuilder& builder, std::uint32_t counter) final {
        builder.append("name", "joe");
        builder.append("hostinfo", 37);
        builder.append("buildinfo", 53);
    }
};

// Test a run of the controller and the data it logs to log file
TEST(FTDCControllerTest, TestFull) {
    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path dir(tempdir.path());

    createDirectoryClean(dir);

    FTDCConfig config;
    config.enabled = true;
    config.period = Milliseconds(1);
    config.maxFileSizeBytes = FTDCConfig::kMaxFileSizeBytesDefault;
    config.maxDirectorySizeBytes = FTDCConfig::kMaxDirectorySizeBytesDefault;

    FTDCController c(dir, config);

    auto c1 = stdx::make_unique<FTDCMetricsCollectorMock2>();
    auto c2 = stdx::make_unique<FTDCMetricsCollectorMockRotate>();

    auto c1Ptr = c1.get();
    auto c2Ptr = c2.get();

    c1Ptr->setSignalOnCount(100);

    c.addPeriodicCollector(std::move(c1));

    c.addOnRotateCollector(std::move(c2));

    c.start();

    // Wait for 100 samples to have occured
    c1Ptr->wait();

    c.stop();

    auto docsPeriodic = c1Ptr->getDocs();
    ASSERT_GREATER_THAN_OR_EQUALS(docsPeriodic.size(), 100UL);

    auto docsRotate = c2Ptr->getDocs();
    ASSERT_EQUALS(docsRotate.size(), 1UL);

    std::vector<BSONObj> allDocs(docsRotate.begin(), docsRotate.end());
    allDocs.insert(allDocs.end(), docsPeriodic.begin(), docsPeriodic.end());

    auto files = scanDirectory(dir);

    ASSERT_EQUALS(files.size(), 1UL);

    auto alog = files[0];

    ValidateDocumentList(alog, allDocs);
}

// Test we can start and stop the controller in quick succession, make sure it succeeds without
// assert or fault
TEST(FTDCControllerTest, TestStartStop) {
    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path dir(tempdir.path());

    createDirectoryClean(dir);

    FTDCConfig config;
    config.enabled = false;
    config.period = Milliseconds(1);
    config.maxFileSizeBytes = FTDCConfig::kMaxFileSizeBytesDefault;
    config.maxDirectorySizeBytes = FTDCConfig::kMaxDirectorySizeBytesDefault;

    FTDCController c(dir, config);

    c.start();

    c.stop();
}

// Test we can start the controller as disabled, the directory is empty, and then we can succesfully
// enable it
TEST(FTDCControllerTest, TestStartAsDisabled) {
    unittest::TempDir tempdir("metrics_testpath");
    boost::filesystem::path dir(tempdir.path());

    createDirectoryClean(dir);

    FTDCConfig config;
    config.enabled = false;
    config.period = Milliseconds(1);
    config.maxFileSizeBytes = FTDCConfig::kMaxFileSizeBytesDefault;
    config.maxDirectorySizeBytes = FTDCConfig::kMaxDirectorySizeBytesDefault;

    auto c1 = stdx::make_unique<FTDCMetricsCollectorMock2>();

    auto c1Ptr = c1.get();

    FTDCController c(dir, config);

    c.addPeriodicCollector(std::move(c1));

    c.start();

    auto files0 = scanDirectory(dir);

    ASSERT_EQUALS(files0.size(), 0UL);

    c.setEnabled(true);

    c1Ptr->setSignalOnCount(50);

    // Wait for 50 samples to have occured
    c1Ptr->wait();

    c.stop();

    auto docsPeriodic = c1Ptr->getDocs();
    ASSERT_GREATER_THAN_OR_EQUALS(docsPeriodic.size(), 50UL);

    std::vector<BSONObj> allDocs(docsPeriodic.begin(), docsPeriodic.end());

    auto files = scanDirectory(dir);

    ASSERT_EQUALS(files.size(), 1UL);

    auto alog = files[0];

    ValidateDocumentList(alog, allDocs);
}

}  // namespace mongo
