// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/executor/egress_networking_parameters_gen.h"
#include "mongo/logv2/log.h"

#include <string_view>

#include <boost/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::executor {

void OpportunisticSecondaryTargetingParameter::append(OperationContext*,
                                                      BSONObjBuilder* b,
                                                      std::string_view name,
                                                      const boost::optional<TenantId>&) {
    return;
}

Status OpportunisticSecondaryTargetingParameter::set(const BSONElement& newValueElement,
                                                     const boost::optional<TenantId>&) {
    LOGV2_WARNING(
        9206304,
        "Opportunistic secondary targeting has been deprecated and the "
        "opportunisticSecondaryTargeting parameter has no effect. For more information please "
        "see https://dochub.mongodb.org/core/hedged-reads-deprecated");

    return Status::OK();
}

Status OpportunisticSecondaryTargetingParameter::setFromString(std::string_view modeStr,
                                                               const boost::optional<TenantId>&) {
    LOGV2_WARNING(
        9206305,
        "Opportunistic secondary targeting has been deprecated and the "
        "opportunisticSecondaryTargeting parameter has no effect. For more information please "
        "see https://dochub.mongodb.org/core/hedged-reads-deprecated");

    return Status::OK();
}

}  // namespace mongo::executor
