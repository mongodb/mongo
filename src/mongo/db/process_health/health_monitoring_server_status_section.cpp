// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/process_health/fault_manager.h"
#include "mongo/db/service_context.h"

#include <memory>

namespace mongo {

class HealthMonitoringServerStatus : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        auto* fault_manager = process_health::FaultManager::get(getGlobalServiceContext());
        if (!fault_manager) {
            return BSONObj();
        }

        BSONObjBuilder result;

        bool appendDetails = false;
        if (configElement.type() == BSONType::object && configElement.Obj().hasElement("details")) {
            appendDetails = configElement.Obj()["details"].trueValue();
        }

        fault_manager->appendDescription(&result, appendDetails);

        return result.obj();
    }
};
auto& healthMonitoringServerStatus =
    *ServerStatusSectionBuilder<HealthMonitoringServerStatus>("health").forRouter();

}  // namespace mongo
