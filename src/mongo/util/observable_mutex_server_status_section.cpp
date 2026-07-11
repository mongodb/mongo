// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/observable_mutex_registry.h"

namespace mongo {

namespace {
class ObservableMutexServerStatusSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return false;
    }

    BSONObj generateSection(OperationContext* opCtx, const BSONElement& config) const override {
        bool listAll = false;
        if (config.isABSONObj()) {
            auto obj = config.Obj();
            if (auto listAllField = obj.getField("listAll"); listAllField.isNumber()) {
                listAll = listAllField.number();
            }
        }

        return ObservableMutexRegistry::get().report(listAll);
    }
};

auto& observableMutexSection =
    *ServerStatusSectionBuilder<ObservableMutexServerStatusSection>("lockContentionMetrics")
         .forShard()
         .forRouter();

}  // namespace

}  // namespace mongo
