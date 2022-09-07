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

#include "mongo/platform/basic.h"

#include "mongo/db/ftdc/collector.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/ftdc/constants.h"
#include "mongo/db/ftdc/util.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/time_support.h"

namespace mongo {

void FTDCCollectorCollection::add(std::unique_ptr<FTDCCollectorInterface> collector) {
    // TODO: ensure the collectors all have unique names.
    _collectors.emplace_back(std::move(collector));
}

std::tuple<BSONObj, Date_t> FTDCCollectorCollection::collect(Client* client) {
    // If there are no collectors, just return an empty BSONObj so that that are caller knows we did
    // not collect anything
    if (_collectors.empty()) {
        return std::tuple<BSONObj, Date_t>(BSONObj(), Date_t());
    }

    BSONObjBuilder builder;

    Date_t start = client->getServiceContext()->getPreciseClockSource()->now();
    Date_t end;
    bool firstLoop = true;

    builder.appendDate(kFTDCCollectStartField, start);

    // All collectors should be ok seeing the inconsistent states in the middle of replication
    // batches. This is desirable because we want to be able to collect data in the middle of
    // batches that are taking a long time.
    auto opCtx = client->makeOperationContext();
    ShouldNotConflictWithSecondaryBatchApplicationBlock shouldNotConflictBlock(opCtx->lockState());
    opCtx->lockState()->setAdmissionPriority(AdmissionContext::Priority::kImmediate);

    for (auto& collector : _collectors) {
        // Skip collection if this collector has no data to return
        if (!collector->hasData()) {
            continue;
        }

        BSONObjBuilder subObjBuilder(builder.subobjStart(collector->name()));

        // Add a Date_t before and after each BSON is collected so that we can track timing of the
        // collector.
        Date_t now = start;

        if (!firstLoop) {
            now = client->getServiceContext()->getPreciseClockSource()->now();
        }

        firstLoop = false;

        subObjBuilder.appendDate(kFTDCCollectStartField, now);

        collector->collect(opCtx.get(), subObjBuilder);

        end = client->getServiceContext()->getPreciseClockSource()->now();
        subObjBuilder.appendDate(kFTDCCollectEndField, end);

        // Ensure the collector did not set a read timestamp.
        invariant(opCtx->recoveryUnit()->getTimestampReadSource() ==
                  RecoveryUnit::ReadSource::kNoTimestamp);
    }

    builder.appendDate(kFTDCCollectEndField, end);

    return std::tuple<BSONObj, Date_t>(builder.obj(), start);
}

}  // namespace mongo
