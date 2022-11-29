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

#include "mongo/db/query/cost_model/cost_model_on_update.h"

#include "mongo/bson/json.h"
#include "mongo/db/client.h"
#include "mongo/db/query/cost_model/cost_model_manager.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::cost_model {

namespace {
BSONObj getCostModelCoefficientsOverride() {
    if (internalCostModelCoefficients.empty()) {
        return BSONObj();
    }

    return fromjson(internalCostModelCoefficients);
}
}  // namespace

Status updateCostCoefficients() {
    if (auto client = Client::getCurrent()) {
        auto serviceCtx = client->getServiceContext();
        tassert(7049000, "ServiceContext must be non null", serviceCtx);

        const auto overrides = getCostModelCoefficientsOverride();
        auto updater = onCoefficientsChangeUpdater(serviceCtx).get();
        if (!updater) {
            return Status(ErrorCodes::IllegalOperation,
                          "failed to set 'internalCostModelCoefficients' because "
                          "OnCoefficientsChangeUpdater is null");
        }
        updater->updateCoefficients(serviceCtx, overrides);
    } else {
        // 'client' may be null if the server parameter is set on mongod startup.
        LOGV2_DEBUG(7049001, 5, "Cost model coefficients updated on startup");
    }

    return Status::OK();
}

const Decorable<ServiceContext>::Decoration<std::unique_ptr<OnCoefficientsChangeUpdater>>
    onCoefficientsChangeUpdater =
        ServiceContext::declareDecoration<std::unique_ptr<OnCoefficientsChangeUpdater>>();
}  // namespace mongo::cost_model
