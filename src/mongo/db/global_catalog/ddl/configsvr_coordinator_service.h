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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/service_context.h"
#include "mongo/util/concurrency/thread_pool.h"

#include <memory>
#include <vector>

namespace mongo {

class ConfigsvrCoordinator;

class ConfigsvrCoordinatorService final : public repl::PrimaryOnlyService {
public:
    static constexpr StringData kServiceName = "ConfigsvrCoordinatorService"_sd;

    explicit ConfigsvrCoordinatorService(ServiceContext* serviceContext)
        : PrimaryOnlyService(serviceContext) {}

    ~ConfigsvrCoordinatorService() override = default;

    static ConfigsvrCoordinatorService* getService(OperationContext* opCtx);

    StringData getServiceName() const override {
        return kServiceName;
    }

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kConfigsvrCoordinatorsNamespace;
    }

    ThreadPool::Limits getThreadPoolLimits() const override {
        return ThreadPool::Limits();
    }

    std::shared_ptr<ConfigsvrCoordinator> getOrCreateService(OperationContext* opCtx,
                                                             BSONObj coorDoc);

    void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) override;

    std::shared_ptr<Instance> constructInstance(BSONObj initialState) override;

    bool areAllCoordinatorsOfTypeFinished(OperationContext* opCtx,
                                          ConfigsvrCoordinatorTypeEnum coordinatorType);

    void waitForAllOngoingCoordinatorsOfType(OperationContext* opCtx,
                                             ConfigsvrCoordinatorTypeEnum coordinatorType);
};

}  // namespace mongo
