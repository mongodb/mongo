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

#pragma once

#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/move_primary/move_primary_metrics.h"
#include "mongo/db/s/move_primary/move_primary_state_machine_gen.h"

namespace mongo {

class MovePrimaryDonorService : public repl::PrimaryOnlyService {
public:
    static constexpr StringData kServiceName = "MovePrimaryDonorService"_sd;

    MovePrimaryDonorService(ServiceContext* serviceContext);

    StringData getServiceName() const override;

    NamespaceString getStateDocumentsNS() const override;

    ThreadPool::Limits getThreadPoolLimits() const override;

    void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const Instance*>& existingInstances) override;

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialState) override;

private:
    ServiceContext* _serviceContext;
};


class MovePrimaryDonor : public repl::PrimaryOnlyService::TypedInstance<MovePrimaryDonor> {
public:
    MovePrimaryDonor(ServiceContext* serviceContext, MovePrimaryDonorDocument initialState);

    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& token) noexcept override;

    void interrupt(Status status) override;

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override;

    void checkIfOptionsConflict(const BSONObj& stateDoc) const override;

    const MovePrimaryCommonMetadata& getMetadata() const;

private:
    ServiceContext* _serviceContext;
    const MovePrimaryCommonMetadata _metadata;
    MovePrimaryDonorMutableFields _mutableFields;
    std::unique_ptr<MovePrimaryMetrics> _metrics;
};

}  // namespace mongo
