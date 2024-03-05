/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <variant>

#include <absl/container/flat_hash_map.h>

#include "mongo/bson/bsonelement.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_provenance.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/server_write_concern_metrics.h"
#include "mongo/db/stats/server_write_concern_metrics_gen.h"
#include "mongo/util/decorable.h"

namespace mongo {

namespace {
const auto ServerWriteConcernMetricsDecoration =
    ServiceContext::declareDecoration<ServerWriteConcernMetrics>();
}  // namespace

ServerWriteConcernMetrics* ServerWriteConcernMetrics::get(ServiceContext* service) {
    return &ServerWriteConcernMetricsDecoration(service);
}

ServerWriteConcernMetrics* ServerWriteConcernMetrics::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void ServerWriteConcernMetrics::recordWriteConcernForInserts(
    const WriteConcernOptions& writeConcernOptions, size_t numInserts) {
    if (!reportOpWriteConcernCountersInServerStatus) {
        return;
    }

    stdx::lock_guard<Latch> lg(_mutex);
    _insertMetrics.recordWriteConcern(writeConcernOptions, numInserts);
}

void ServerWriteConcernMetrics::recordWriteConcernForUpdate(
    const WriteConcernOptions& writeConcernOptions) {
    if (!reportOpWriteConcernCountersInServerStatus) {
        return;
    }

    stdx::lock_guard<Latch> lg(_mutex);
    _updateMetrics.recordWriteConcern(writeConcernOptions);
}

void ServerWriteConcernMetrics::recordWriteConcernForDelete(
    const WriteConcernOptions& writeConcernOptions) {
    if (!reportOpWriteConcernCountersInServerStatus) {
        return;
    }

    stdx::lock_guard<Latch> lg(_mutex);
    _deleteMetrics.recordWriteConcern(writeConcernOptions);
}

BSONObj ServerWriteConcernMetrics::toBSON() const {
    if (!reportOpWriteConcernCountersInServerStatus) {
        return BSONObj();
    }

    stdx::lock_guard<Latch> lg(_mutex);

    BSONObjBuilder builder;

    BSONObjBuilder insertBuilder(builder.subobjStart("insert"));
    _insertMetrics.toBSON(&insertBuilder);
    insertBuilder.done();

    BSONObjBuilder updateBuilder(builder.subobjStart("update"));
    _updateMetrics.toBSON(&updateBuilder);
    updateBuilder.done();

    BSONObjBuilder deleteBuilder(builder.subobjStart("delete"));
    _deleteMetrics.toBSON(&deleteBuilder);
    deleteBuilder.done();

    return builder.obj();
}

void ServerWriteConcernMetrics::WriteConcernCounters::recordWriteConcern(
    const WriteConcernOptions& writeConcernOptions, size_t numOps) {
    if (auto wMode = get_if<std::string>(&writeConcernOptions.w)) {
        if (writeConcernOptions.isMajority()) {
            wMajorityCount += numOps;
            return;
        }

        wTagCounts[*wMode] += numOps;
        return;
    }

    if (holds_alternative<WTags>(writeConcernOptions.w)) {
        // wTags is an internal feature that we don't track metrics for
        return;
    }

    wNumCounts[std::get<int64_t>(writeConcernOptions.w)] += numOps;
}

void ServerWriteConcernMetrics::WriteConcernMetricsForOperationType::recordWriteConcern(
    const WriteConcernOptions& writeConcernOptions, size_t numOps) {
    if (writeConcernOptions.notExplicitWValue) {
        if (writeConcernOptions.getProvenance().isCustomDefault()) {
            cWWC.recordWriteConcern(writeConcernOptions, numOps);
        } else {
            // Provenance is either:
            //  - "implicitDefault" : implicit default WC (w:1 or w:"majority") is used.
            //  - "internalWriteDefault" : if internal command sets empty WC ({writeConcern: {}}),
            //    then default constructed WC (w:1) is used.
            implicitDefaultWC.recordWriteConcern(writeConcernOptions, numOps);
        }

        notExplicitWCount += numOps;
    } else {
        // Supplied write concern contains 'w' field, the provenance can still be default if it is
        // being set by mongos.
        if (writeConcernOptions.getProvenance().isCustomDefault()) {
            cWWC.recordWriteConcern(writeConcernOptions, numOps);
            notExplicitWCount += numOps;
        } else if (writeConcernOptions.getProvenance().isImplicitDefault()) {
            implicitDefaultWC.recordWriteConcern(writeConcernOptions, numOps);
            notExplicitWCount += numOps;
        } else {
            explicitWC.recordWriteConcern(writeConcernOptions, numOps);
        }
    }
}

void ServerWriteConcernMetrics::WriteConcernCounters::toBSON(BSONObjBuilder* builder) const {
    builder->append("wmajority", static_cast<long long>(wMajorityCount));

    BSONObjBuilder wNumBuilder(builder->subobjStart("wnum"));
    for (auto const& pair : wNumCounts) {
        wNumBuilder.append(std::to_string(pair.first), static_cast<long long>(pair.second));
    }
    wNumBuilder.done();

    if (exportWTag) {
        BSONObjBuilder wTagBuilder(builder->subobjStart("wtag"));
        for (auto const& pair : wTagCounts) {
            wTagBuilder.append(pair.first, static_cast<long long>(pair.second));
        }
        wTagBuilder.done();
    }
}

void ServerWriteConcernMetrics::WriteConcernMetricsForOperationType::toBSON(
    BSONObjBuilder* builder) const {
    explicitWC.toBSON(builder);

    builder->append("none", static_cast<long long>(notExplicitWCount));
    BSONObjBuilder noneBuilder(builder->subobjStart("noneInfo"));

    BSONObjBuilder cWWCBuilder(noneBuilder.subobjStart("CWWC"));
    cWWC.toBSON(&cWWCBuilder);
    cWWCBuilder.done();

    BSONObjBuilder implicitBuilder(noneBuilder.subobjStart("implicitDefault"));
    implicitDefaultWC.toBSON(&implicitBuilder);
    implicitBuilder.done();

    noneBuilder.done();
}

namespace {
class OpWriteConcernCountersSSS : public ServerStatusSection {
public:
    using ServerStatusSection::ServerStatusSection;

    ~OpWriteConcernCountersSSS() override = default;

    bool includeByDefault() const override {
        // When 'reportOpWriteConcernCountersInServerStatus' is false, do not include this section
        // unless requested by the user. Even if the user requests the section, it will not be
        // included because an empty BSONObj is generated for the section.
        return reportOpWriteConcernCountersInServerStatus;
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        return ServerWriteConcernMetrics::get(opCtx)->toBSON();
    }
};
auto& opWriteConcernCountersSSS =
    *ServerStatusSectionBuilder<OpWriteConcernCountersSSS>("opWriteConcernCounters");
}  // namespace

}  // namespace mongo
