// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/ddl/sharding_coordinator.h"
#include "mongo/db/shard_role/ddl/ddl_lock_manager.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding
namespace mongo {

class [[MONGO_MOD_PRIVATE]] ShardingDDLCoordinatorMixin {
protected:
    explicit ShardingDDLCoordinatorMixin(const BSONObj& coorDoc);
    virtual ~ShardingDDLCoordinatorMixin() = default;

    void _initializeLockerAndCheckAllowedToStart(ShardingCoordinator& self,
                                                 OperationContext* opCtx);

    const boost::optional<mongo::DatabaseVersion>& getDatabaseVersion() const;

    void _checkDBVersion(ShardingCoordinator& self,
                         OperationContext* opCtx,
                         bool afterAcquiringLocks);

    virtual std::set<NamespaceString> _getAdditionalLocksToAcquire(OperationContext* opCtx);

    ExecutorFuture<void> _acquireAllDDLLocksAsync(
        ShardingCoordinator& self,
        OperationContext* opCtx,
        std::shared_ptr<executor::ScopedTaskExecutor> executor,
        const CancellationToken& token);

    void _releaseDDLLocks(OperationContext* opCtx);

private:
    template <typename T>
    ExecutorFuture<void> _acquireDDLLockAsync(
        ShardingCoordinator& self,
        std::shared_ptr<executor::ScopedTaskExecutor> executor,
        const CancellationToken& token,
        const T& resource,
        LockMode lockMode);

    const boost::optional<mongo::DatabaseVersion> _databaseVersion;
    // A Locker object works attached to an opCtx and it's destroyed once the opCtx gets out of
    // scope. However, we must keep alive a unique Locker object during the whole
    // ShardingCoordinator life to preserve the lock state among all the executor tasks.
    std::unique_ptr<Locker> _locker;
    std::stack<DDLLockManager::ScopedBaseDDLLock> _scopedLocks;

    friend class ShardingDDLCoordinatorTest;
};

template <typename StateDoc>
class [[MONGO_MOD_NEEDS_REPLACEMENT]] NonRecoverableShardingDDLCoordinator
    : public ShardingCoordinator,
      protected NonRecoverableTypedDocMixin<StateDoc>,
      protected ShardingDDLCoordinatorMixin {
public:
    using ShardingDDLCoordinatorMixin::getDatabaseVersion;

protected:
    explicit NonRecoverableShardingDDLCoordinator(ShardingCoordinatorService* service,
                                                  std::string name,
                                                  const BSONObj& coorDoc)
        : ShardingCoordinator(service, std::move(name), coorDoc),
          NonRecoverableTypedDocMixin<StateDoc>(coorDoc),
          ShardingDDLCoordinatorMixin(coorDoc) {}

    const CoordinatorStateDoc& getDoc() const override {
        return this->_docWrapper;
    }

    CoordinatorStateDoc& getDoc() override {
        return this->_docWrapper;
    }

private:
    void _initialize(OperationContext* opCtx) override {
        this->_initializeLockerAndCheckAllowedToStart(*this, opCtx);
    }

    void _checkCoordinatorPreconditions(OperationContext* opCtx, bool afterAcquiringLocks) final {
        this->_checkDBVersion(*this, opCtx, afterAcquiringLocks);
    }

    ExecutorFuture<void> _acquireLocksAsync(OperationContext* opCtx,
                                            std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                            const CancellationToken& token) final {
        return this->_acquireAllDDLLocksAsync(*this, opCtx, executor, token);
    }

    void _releaseLocks(OperationContext* opCtx) override {
        this->_releaseDDLLocks(opCtx);
    }

    bool _isInCriticalSectionGeneric(CoordinatorGenericPhase phase) const final {
        return false;
    }

    friend class ShardingDDLCoordinatorTest;
};

template <typename StateDoc>
class [[MONGO_MOD_UNFORTUNATELY_OPEN]] RecoverableShardingDDLCoordinator
    : public RecoverableShardingCoordinator,
      protected RecoverableTypedDocMixin<RecoverableShardingDDLCoordinator<StateDoc>, StateDoc>,
      protected ShardingDDLCoordinatorMixin {

    friend RecoverableTypedDocMixin<RecoverableShardingDDLCoordinator<StateDoc>, StateDoc>;

public:
    using ShardingDDLCoordinatorMixin::getDatabaseVersion;

protected:
    explicit RecoverableShardingDDLCoordinator(ShardingCoordinatorService* service,
                                               std::string name,
                                               const BSONObj& coorDoc)
        : RecoverableShardingCoordinator(service, std::move(name), coorDoc),
          RecoverableTypedDocMixin<RecoverableShardingDDLCoordinator<StateDoc>, StateDoc>(coorDoc),
          ShardingDDLCoordinatorMixin(coorDoc) {}

    const CoordinatorStateDoc& getDoc() const override {
        return this->_docWrapper;
    }

    CoordinatorStateDoc& getDoc() override {
        return this->_docWrapper;
    }

private:
    void _initialize(OperationContext* opCtx) override {
        this->_initializeLockerAndCheckAllowedToStart(*this, opCtx);
    }

    virtual void checkDBVersion(OperationContext* opCtx, bool afterAcquiringLocks) {
        this->_checkDBVersion(*this, opCtx, afterAcquiringLocks);
    }

    void _checkCoordinatorPreconditions(OperationContext* opCtx, bool afterAcquiringLocks) final {
        checkDBVersion(opCtx, afterAcquiringLocks);
    }

    ExecutorFuture<void> _acquireLocksAsync(OperationContext* opCtx,
                                            std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                            const CancellationToken& token) final {
        return this->_acquireAllDDLLocksAsync(*this, opCtx, executor, token);
    }

    void _releaseLocks(OperationContext* opCtx) override {
        this->_releaseDDLLocks(opCtx);
    }

    std::string_view serializeGenericPhase(CoordinatorGenericPhase phase) const final {
        return this->serializePhase(phase);
    }

    bool _isInCriticalSectionGeneric(CoordinatorGenericPhase phase) const final {
        return this->isInCriticalSection(
            CoordinatorStateDocImpl<StateDoc>::castToCoordinatorPhase(phase));
    }
};

}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
