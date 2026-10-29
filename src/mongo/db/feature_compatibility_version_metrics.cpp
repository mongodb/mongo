// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status/server_status.h"
#include "mongo/db/feature_compatibility_version_document_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/version/releases.h"

#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class FCVServerStatusMetrics : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    ~FCVServerStatusMetrics() override = default;

    bool includeByDefault() const override {
        return true;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        BSONObjBuilder bob;
        serverGlobalParams.featureCompatibility.withAcquiredFCVDocument(
            [&](const FeatureCompatibilityVersionDocument* fcvDoc) {
                if (!fcvDoc) {
                    return;
                }
                auto to = fcvDoc->getTargetVersion();
                auto version = to.value_or(fcvDoc->getVersion());
                bob.append("major", multiversion::majorVersion(version));
                bob.append("minor", multiversion::minorVersion(version));

                int currentlyTransitioning = 0;
                if (to) {
                    auto from = fcvDoc->getPreviousVersion().value_or(fcvDoc->getVersion());
                    // from is greater, we are downgrading
                    currentlyTransitioning = (from > *to) ? -1 : 1;
                }
                bob.append("transitioning", currentlyTransitioning);
            });

        return bob.obj();
    }
};
auto& fcvServerStatusMetrics =
    *ServerStatusSectionBuilder<FCVServerStatusMetrics>("featureCompatibilityVersion").forShard();

}  // namespace
}  // namespace mongo
