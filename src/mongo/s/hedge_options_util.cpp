/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/s/hedge_options_util.h"

#include "mongo/client/read_preference.h"
#include "mongo/db/query/query_request.h"
#include "mongo/s/mongos_server_parameters_gen.h"

namespace mongo {

boost::optional<executor::RemoteCommandRequestOnAny::HedgeOptions> extractHedgeOptions(
    OperationContext* opCtx, const BSONObj& cmdObj) {
    const auto hedgingMode = ReadPreferenceSetting::get(opCtx).hedgingMode;

    if (gReadHedgingMode.load() == ReadHedgingMode::kOn && hedgingMode &&
        hedgingMode->getEnabled()) {
        boost::optional<int> maxTimeMS;
        if (auto cmdOptionMaxTimeMSField = cmdObj[QueryRequest::cmdOptionMaxTimeMS]) {
            maxTimeMS = uassertStatusOK(QueryRequest::parseMaxTimeMS(cmdOptionMaxTimeMSField));
        }

        // Check if the operation is worth hedging.
        if (maxTimeMS && maxTimeMS > gMaxTimeMSThresholdForHedging.load()) {
            return boost::none;
        }

        // Compute the delay.
        auto delay = Milliseconds{0};
        bool shouldDelayHedging = hedgingMode->getDelay();

        if (shouldDelayHedging) {
            delay = maxTimeMS ? Milliseconds{gHedgingDelayPercentage.load() * maxTimeMS.get() / 100}
                              : Milliseconds{gDefaultHedgingDelayMS.load()};
        }

        return executor::RemoteCommandRequestOnAny::HedgeOptions{1, delay};
    }

    return boost::none;
}

}  // namespace mongo
