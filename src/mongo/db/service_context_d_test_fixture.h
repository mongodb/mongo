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

#include "mongo/base/checked_cast.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/journal_listener.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/duration.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/tick_source_mock.h"

#include <memory>
#include <string>
#include <utility>

namespace mongo {

class MongoDScopedGlobalServiceContextForTest : public ScopedGlobalServiceContextForTest {
public:
    constexpr static StorageEngineInitFlags kDefaultStorageEngineInitFlags =
        StorageEngineInitFlags::kAllowNoLockFile | StorageEngineInitFlags::kSkipMetadataFile;

    enum class RepairAction { kNoRepair, kRepair };

    class Options {
    public:
        Options() = default;

        Options(Options&&) = default;
        Options& operator=(Options&&) = default;

        Options engine(std::string engine) {
            _engine = std::move(engine);
            return std::move(*this);
        }
        Options enableRepair() {
            _repair = RepairAction::kRepair;
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

        Options inMemory(bool inMemory) {
            _inMemory = inMemory;
            return std::move(*this);
        }

        Options forceDisableTableLogging() {
            _forceDisableTableLogging = true;
            return std::move(*this);
        }

        Options useIndexBuildsCoordinator(
            std::unique_ptr<IndexBuildsCoordinator> indexBuildsCoordinator) {
            _indexBuildsCoordinator = std::move(indexBuildsCoordinator);
            return std::move(*this);
        }

        Options setCreateShardingState(bool createShardingState) {
            _createShardingState = createShardingState;
            return std::move(*this);
        }

        Options setAuthEnabled(bool enableAuth) {
            _setAuthEnabled = enableAuth;
            return std::move(*this);
        }

        Options setAuthObjects(bool setAuthObjects) {
            _setAuthObjects = setAuthObjects;
            return std::move(*this);
        }

        Options addClientObserver(std::unique_ptr<ServiceContext::ClientObserver> observer) {
            _clientObservers.emplace_back(std::move(observer));
            return std::move(*this);
        }

        template <typename T>
        Options setParameter(StringData parameter, T value) {
            _parameters.emplace_back(parameter, value);
            return std::move(*this);
        }

    private:
        friend class MongoDScopedGlobalServiceContextForTest;

        std::string _engine = "wiredTiger";
        // We use in-memory instances by default to advise Storage Engines (in particular
        // WiredTiger) not to perform Disk I/O.
        bool _inMemory = true;
        RepairAction _repair = RepairAction::kNoRepair;
        StorageEngineInitFlags _initFlags = kDefaultStorageEngineInitFlags;
        bool _useReplSettings = false;
        bool _useMockClock = false;
        bool _setAuthEnabled = true;
        bool _setAuthObjects = false;
        Milliseconds _autoAdvancingMockClockIncrement{0};
        std::unique_ptr<TickSource> _mockTickSource;
        std::unique_ptr<JournalListener> _journalListener;
        std::unique_ptr<IndexBuildsCoordinator> _indexBuildsCoordinator;
        bool _forceDisableTableLogging = false;
        bool _createShardingState = true;
        std::vector<std::unique_ptr<ServiceContext::ClientObserver>> _clientObservers;
        std::vector<RAIIServerParameterControllerForTest> _parameters;
    };

    MongoDScopedGlobalServiceContextForTest(Options options, bool shouldSetupTL);
    MongoDScopedGlobalServiceContextForTest(
        ServiceContext::UniqueServiceContext serviceContextHolder,
        Options options,
        bool shouldSetupTL);

    ~MongoDScopedGlobalServiceContextForTest() override;

    JournalListener* journalListener() const {
        return _journalListener.get();
    }

    OpObserverRegistry* opObserverRegistry() {
        return _opObserverRegistry;
    }

private:
    static ServiceContext::UniqueServiceContext makeServiceContext(
        bool useMockClock,
        Milliseconds autoAdvancingMockClockIncrement,
        std::unique_ptr<TickSource> tickSource);

    // The JournalListener must stay alive as long as the storage engine is running.
    std::unique_ptr<JournalListener> _journalListener;

    OpObserverRegistry* _opObserverRegistry;

    struct {
        std::string engine;
        bool engineSetByUser;
        bool repair;
    } _stashedStorageParams;

    unittest::TempDir _tempDir{"service_context_d_test_fixture"};
};

class ServiceContextMongoDTest : public ServiceContextTest {
public:
    using Options = MongoDScopedGlobalServiceContextForTest::Options;

    ServiceContextMongoDTest() : ServiceContextMongoDTest{Options{}} {}
    explicit ServiceContextMongoDTest(
        std::unique_ptr<MongoDScopedGlobalServiceContextForTest> scopedServiceContext)
        : ServiceContextTest(std::move(scopedServiceContext)) {}
    explicit ServiceContextMongoDTest(Options options)
        : ServiceContextTest(std::make_unique<MongoDScopedGlobalServiceContextForTest>(
              std::move(options), shouldSetupTL)) {}

    JournalListener* journalListener() const {
        return mongoDscopedServiceContext()->journalListener();
    }

    OpObserverRegistry* opObserverRegistry() {
        return mongoDscopedServiceContext()->opObserverRegistry();
    }

private:
    MongoDScopedGlobalServiceContextForTest* mongoDscopedServiceContext() const {
        return checked_cast<MongoDScopedGlobalServiceContextForTest*>(scopedServiceContext());
    }
};

}  // namespace mongo
