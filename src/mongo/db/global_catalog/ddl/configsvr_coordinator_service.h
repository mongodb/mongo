// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/ddl/configsvr_coordinator_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/service_context.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <vector>

namespace mongo {
using namespace std::literals::string_view_literals;

class ConfigsvrCoordinator;

class [[MONGO_MOD_NEEDS_REPLACEMENT]] ConfigsvrCoordinatorService final
    : public repl::PrimaryOnlyService {
public:
    static constexpr std::string_view kServiceName = "ConfigsvrCoordinatorService"sv;

    explicit ConfigsvrCoordinatorService(ServiceContext* serviceContext)
        : PrimaryOnlyService(serviceContext) {}

    ~ConfigsvrCoordinatorService() override = default;

    static ConfigsvrCoordinatorService* getService(OperationContext* opCtx);

    std::string_view getServiceName() const override {
        return kServiceName;
    }

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kConfigsvrCoordinatorsNamespace;
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
