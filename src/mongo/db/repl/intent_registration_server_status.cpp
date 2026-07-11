// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/intent_registry.h"

namespace mongo {
namespace repl {
namespace {

class IntentRegistryServerStatusSection : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        auto& intentRegistry = rss::consensus::IntentRegistry::get(opCtx->getServiceContext());

        BSONObjBuilder result;

        for (size_t i = 0; i < intentRegistry.getTotalIntentsDeclared().size(); ++i) {
            auto msg = "intentsDeclaredFor" +
                intentRegistry.intentToString(
                    static_cast<rss::consensus::IntentRegistry::Intent>(i));
            result.append(msg, static_cast<int>(intentRegistry.getTotalIntentsDeclared()[i]));
        }

        result.done();

        return result.obj();
    }
};

auto& intentRegistryServerStatusSection =
    *ServerStatusSectionBuilder<IntentRegistryServerStatusSection>("intentRegistry").forShard();

}  // namespace
}  // namespace repl
}  // namespace mongo
