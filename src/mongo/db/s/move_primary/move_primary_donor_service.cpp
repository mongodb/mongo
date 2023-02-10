/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/s/move_primary/move_primary_donor_service.h"

#include "mongo/db/s/move_primary/move_primary_server_parameters_gen.h"

namespace mongo {

MovePrimaryDonorService::MovePrimaryDonorService(ServiceContext* serviceContext)
    : PrimaryOnlyService{serviceContext} {}

StringData MovePrimaryDonorService::getServiceName() const {
    return kServiceName;
}

NamespaceString MovePrimaryDonorService::getStateDocumentsNS() const {
    return NamespaceString::kTenantMigrationDonorsNamespace;
}

ThreadPool::Limits MovePrimaryDonorService::getThreadPoolLimits() const {
    ThreadPool::Limits limits;
    limits.minThreads = gMovePrimaryDonorServiceMinThreadCount;
    limits.maxThreads = gMovePrimaryDonorServiceMaxThreadCount;
    return limits;
}

void MovePrimaryDonorService::checkIfConflictsWithOtherInstances(
    OperationContext* opCtx,
    BSONObj initialState,
    const std::vector<const repl::PrimaryOnlyService::Instance*>& existingInstances) {
    auto initialDoc = MovePrimaryDonorDocument::parse(
        IDLParserContext("MovePrimaryDonorCheckIfConflictsWithOtherInstances"), initialState);
    const auto& newMetadata = initialDoc.getMetadata();
    for (const auto& instance : existingInstances) {
        auto typed = checked_cast<const MovePrimaryDonorService::Instance*>(instance);
        const auto& existingMetadata = typed->getMetadata();
        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Existing movePrimary operation for database "
                              << newMetadata.getDatabaseName() << " is still ongoing",
                newMetadata.getDatabaseName() != existingMetadata.getDatabaseName());
    }
}

std::shared_ptr<repl::PrimaryOnlyService::Instance> MovePrimaryDonorService::constructInstance(
    BSONObj initialState) {
    return std::make_shared<MovePrimaryDonorService::Instance>(MovePrimaryDonorDocument::parse(
        IDLParserContext("MovePrimaryDonorConstructInstance"), initialState));
}

SemiFuture<void> MovePrimaryDonorService::Instance::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return SemiFuture<void>(Status::OK());
}

void MovePrimaryDonorService::Instance::interrupt(Status status) {}

boost::optional<BSONObj> MovePrimaryDonorService::Instance::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {
    return boost::none;
}

void MovePrimaryDonorService::Instance::checkIfOptionsConflict(const BSONObj& stateDoc) const {
    auto otherDoc = MovePrimaryDonorDocument::parse(
        IDLParserContext("MovePrimaryDonorCheckIfOptionsConflict"), stateDoc);
    const auto& otherMetadata = otherDoc.getMetadata();
    const auto& metadata = getMetadata();
    invariant(metadata.getId() == otherMetadata.getId());
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Existing movePrimary operation exists with same id, but incompatible arguments",
            metadata.getDatabaseName() == otherMetadata.getDatabaseName() &&
                metadata.getToShard() == otherMetadata.getToShard());
}

MovePrimaryDonorService::Instance::Instance(MovePrimaryDonorDocument initialState)
    : _metadata{std::move(initialState.getMetadata())},
      _mutableFields{std::move(initialState.getMutableFields())} {}

const MovePrimaryDonorMetadata& MovePrimaryDonorService::Instance::getMetadata() const {
    return _metadata;
}

}  // namespace mongo
