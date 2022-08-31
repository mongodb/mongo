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

#pragma once

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {

class ServiceContextMongoDTest : public virtual ServiceContextTest {
public:
    constexpr static StorageEngineInitFlags kDefaultStorageEngineInitFlags =
        StorageEngineInitFlags::kAllowNoLockFile | StorageEngineInitFlags::kSkipMetadataFile;

protected:
    enum class RepairAction { kNoRepair, kRepair };

    class Options {
    public:
        Options(){};

        Options engine(std::string engine) {
            _engine = std::move(engine);
            return std::move(*this);
        }
        Options repair(RepairAction repair) {
            _repair = repair;
            return std::move(*this);
        }
        Options initFlags(StorageEngineInitFlags initFlags) {
            _initFlags = initFlags;
            return std::move(*this);
        }
        Options useReplSettings(bool useReplSettings) {
            _useReplSettings = useReplSettings;
            return std::move(*this);
        }
        Options useMockClock(bool useMockClock, Milliseconds autoAdvance = Milliseconds{0}) {
            _useMockClock = useMockClock;
            _autoAdvancingMockClockIncrement = autoAdvance;
            return std::move(*this);
        }
        template <class D = Milliseconds>
        Options useMockTickSource(bool useMockTickSource) {
            if (useMockTickSource) {
                _mockTickSource = std::make_unique<TickSourceMock<D>>();
            }
            return std::move(*this);
        }
        Options useJournalListener(std::unique_ptr<JournalListener> journalListener) {
            _journalListener = std::move(journalListener);
            return std::move(*this);
        }

        Options ephemeral(bool ephemeral) {
            _ephemeral = ephemeral;
            return std::move(*this);
        }

        Options forceDisableTableLogging() {
            _forceDisableTableLogging = true;
            return std::move(*this);
        }

    private:
        std::string _engine = "wiredTiger";
        // We use ephemeral instances by default to advise Storage Engines (in particular
        // WiredTiger) not to perform Disk I/O.
        bool _ephemeral = true;
        RepairAction _repair = RepairAction::kNoRepair;
        StorageEngineInitFlags _initFlags = kDefaultStorageEngineInitFlags;
        bool _useReplSettings = false;
        bool _useMockClock = false;
        Milliseconds _autoAdvancingMockClockIncrement{0};
        std::unique_ptr<TickSource> _mockTickSource;
        std::unique_ptr<JournalListener> _journalListener;
        bool _forceDisableTableLogging = false;

        friend class ServiceContextMongoDTest;
    };

    explicit ServiceContextMongoDTest(Options options = {});

    virtual ~ServiceContextMongoDTest();

    void tearDown() override;

    // The JournalListener must stay alive as long as the storage engine is running.
    std::unique_ptr<JournalListener> _journalListener;

private:
    struct {
        std::string engine;
        bool engineSetByUser;
        bool repair;
    } _stashedStorageParams;

    struct {
        bool enableMajorityReadConcern;
    } _stashedServerParams;

    unittest::TempDir _tempDir;
};

}  // namespace mongo
