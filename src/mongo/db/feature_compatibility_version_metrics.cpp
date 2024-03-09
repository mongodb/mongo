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

#include <memory>

#include <boost/optional/optional.hpp>

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/version/releases.h"

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
        const ServerGlobalParams::FCVSnapshot fcvSnapshot =
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        if (fcvSnapshot.isVersionInitialized()) {
            bob.append("major", multiversion::majorVersion(fcvSnapshot.getVersion()));
            bob.append("minor", multiversion::minorVersion(fcvSnapshot.getVersion()));

            int currentlyTransitioning = 0;
            // (Generic FCV reference): append information to serverStatus on if we are in a state
            // of transitioning to a new FCV (upgrading or downgrading).
            if (fcvSnapshot.isUpgradingOrDowngrading()) {
                const auto& [fromVersion, toVersion] =
                    multiversion::getTransitionFCVFromAndTo(fcvSnapshot.getVersion());
                currentlyTransitioning = 1;
                // from is greater, we are downgrading
                if (fromVersion > toVersion) {
                    currentlyTransitioning = -1;
                }
            }
            bob.append("transitioning", currentlyTransitioning);
        }

        return bob.obj();
    }
};
auto& fcvServerStatusMetrics =
    *ServerStatusSectionBuilder<FCVServerStatusMetrics>("featureCompatibilityVersion").forShard();

}  // namespace
}  // namespace mongo
