/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/set_change_stream_state_coordinator.h"

#include "mongo/db/change_stream_change_collection_manager.h"
#include "mongo/db/change_stream_pre_images_collection_manager.h"
#include "mongo/db/change_stream_state_gen.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

// Hangs the 'SetChangeStreamStateCoordinator' before calling the
// 'ChangeStreamStateCommandProcessor'.
MONGO_FAIL_POINT_DEFINE(hangSetChangeStreamStateCoordinatorBeforeCommandProcessor);
namespace {

constexpr auto kSetChangeStreamStateCoordinatorServiceName =
    "setChangeStreamStateCoordinatorService"_sd;
constexpr auto kSetChangeStreamStateCoordinatorName = "setChangeStreamStateCoordinator"_sd;

/**
 * Waits until the oplog entry has been majority committed.
 */
void waitForMajority(OperationContext* opCtx) {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    const auto lastLocalOpTime = replCoord->getMyLastAppliedOpTime();
    WaitForMajorityService::get(opCtx->getServiceContext())
        .waitUntilMajority(lastLocalOpTime, opCtx->getCancellationToken())
        .get(opCtx);
}

/**
 * A helper that inspect the the 'SetChangeStreamStateCoordinatorDocument' state document and
 * processes the provided change stream command accordingly.
 */
class ChangeStreamStateCommandProcessor {
public:
    explicit ChangeStreamStateCommandProcessor(SetChangeStreamStateCoordinatorDocument stateDoc)
        : _stateDoc(stateDoc) {}

    /**
     * Retrieves the tenant id and processes the change stream command.
     */
    void process(OperationContext* opCtx) {
        const auto setChangeStreamParameter = ChangeStreamStateParameters::parse(
            IDLParserContext("ChangeStreamStateParameters"), _stateDoc.getCommand());

        invariant(_stateDoc.getId().getTenantId());

        // TODO SERVER-65950 replace 'tenantId' with the provided tenant id.
        auto tenantId = boost::none;

        if (setChangeStreamParameter.getEnabled()) {
            _enableChangeStream(opCtx, tenantId);
        } else {
            _disableChangeStream(opCtx, tenantId);
        }
    }

private:
    /**
     * Enables the change stream in the serverless by creating the change collection and the
     * pre-image collection.
     */
    void _enableChangeStream(OperationContext* opCtx, boost::optional<TenantId> tenantId) {
        auto& changeCollectionManager = ChangeStreamChangeCollectionManager::get(opCtx);
        changeCollectionManager.createChangeCollection(opCtx, tenantId);

        ChangeStreamPreImagesCollectionManager::createPreImagesCollection(opCtx, tenantId);

        // Wait until the create requests are majority committed.
        waitForMajority(opCtx);
    }

    /**
     * Disables the change stream in the serverless by dropping the change collection and the
     * pre-image collection.
     */
    void _disableChangeStream(OperationContext* opCtx, boost::optional<TenantId> tenantId) {
        auto& changeCollectionManager = ChangeStreamChangeCollectionManager::get(opCtx);
        changeCollectionManager.dropChangeCollection(opCtx, tenantId);

        ChangeStreamPreImagesCollectionManager::dropPreImagesCollection(opCtx, tenantId);

        // Wait until the drop requests are majority committed.
        waitForMajority(opCtx);
    }

    const SetChangeStreamStateCoordinatorDocument _stateDoc;
};

}  // namespace

StringData SetChangeStreamStateCoordinator::getInstanceName() {
    return kSetChangeStreamStateCoordinatorName;
}

SetChangeStreamStateCoordinator::SetChangeStreamStateCoordinator(const BSONObj& stateDoc)
    : _stateDoc{SetChangeStreamStateCoordinatorDocument::parse(
          IDLParserContext("SetChangeStreamStateCoordinatorDocument"), stateDoc)},
      _stateDocStore{NamespaceString::kSetChangeStreamStateCoordinatorNamespace} {}

boost::optional<BSONObj> SetChangeStreamStateCoordinator::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {
    // Tenant id must always be present.
    invariant(_stateDoc.getId().getTenantId());

    BSONObjBuilder bob;
    bob.append("tenantId", _stateDoc.getId().getTenantId()->toString());
    bob.append("command", _stateDoc.getCommand());
    return bob.obj();
}

void SetChangeStreamStateCoordinator::checkIfOptionsConflict(const BSONObj& otherDocBSON) const {
    const auto otherDoc = SetChangeStreamStateCoordinatorDocument::parse(
        IDLParserContext("SetChangeStreamStateCoordinatorDocument"), otherDocBSON);

    // The '_id' field of the state document corresponds to the tenant id and hence if we are here
    // then the current and the new request belongs to the same tenant. Reject the new request if it
    // is not identical to the previous one.
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Another conflicting request is in progress",
            SimpleBSONObjComparator::kInstance.evaluate(_stateDoc.toBSON() == otherDoc.toBSON()));
}

ExecutorFuture<void> SetChangeStreamStateCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {

    return ExecutorFuture<void>(**executor).then([this, anchor = shared_from_this()] {
        auto opCtxHolder = cc().makeOperationContext();
        auto* opCtx = opCtxHolder.get();

        try {
            _stateDocStore.add(opCtx, _stateDoc);
        } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
            // A series of retries, step-up and step-down events can cause a node to try and insert
            // the document when it has already been persisted locally, but we must still wait for
            // majority commit.
            waitForMajority(opCtx);
        }

        hangSetChangeStreamStateCoordinatorBeforeCommandProcessor.pauseWhileSet(
            Interruptible::notInterruptible());

        // Dispatch the state document to be processed.
        ChangeStreamStateCommandProcessor commandProcessor(_stateDoc);
        commandProcessor.process(opCtx);
    });
}

void SetChangeStreamStateCoordinator::_removeStateDocument(OperationContext* opCtx) {
    _stateDocStore.remove(
        opCtx,
        BSON(SetChangeStreamStateCoordinatorDocument::kIdFieldName << _stateDoc.getId().toBSON()));
}

StringData SetChangeStreamStateCoordinatorService::getServiceName() const {
    return kSetChangeStreamStateCoordinatorServiceName;
}

NamespaceString SetChangeStreamStateCoordinatorService::getStateDocumentsNS() const {
    return NamespaceString::kSetChangeStreamStateCoordinatorNamespace;
}

ThreadPool::Limits SetChangeStreamStateCoordinatorService::getThreadPoolLimits() const {
    return ThreadPool::Limits();
}

SetChangeStreamStateCoordinatorService* SetChangeStreamStateCoordinatorService::getService(
    OperationContext* opCtx) {
    auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
    auto service = registry->lookupServiceByName(kSetChangeStreamStateCoordinatorServiceName);
    return checked_cast<SetChangeStreamStateCoordinatorService*>(std::move(service));
}

std::shared_ptr<repl::PrimaryOnlyService::Instance>
SetChangeStreamStateCoordinatorService::constructInstance(BSONObj stateDoc) {
    return std::make_shared<SetChangeStreamStateCoordinator>(std::move(stateDoc));
}

std::shared_ptr<SetChangeStreamStateCoordinator>
SetChangeStreamStateCoordinatorService::getOrCreateInstance(OperationContext* opCtx,
                                                            BSONObj coorDoc) {
    return [&] {
        try {
            auto [coordinator, _] = PrimaryOnlyService::getOrCreateInstance(opCtx, coorDoc, true);
            return checked_pointer_cast<SetChangeStreamStateCoordinator>(std::move(coordinator));
        } catch (const DBException& ex) {
            LOGV2_ERROR(6663106,
                        "Failed to create coordinator instance",
                        "service"_attr = kSetChangeStreamStateCoordinatorServiceName,
                        "reason"_attr = ex);
            throw;
        }
    }();
}

}  // namespace mongo
