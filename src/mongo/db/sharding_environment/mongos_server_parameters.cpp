// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/sharding_environment/mongos_server_parameters_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

void ReadHedgingModeParameter::append(OperationContext*,
                                      BSONObjBuilder* builder,
                                      std::string_view name,
                                      const boost::optional<TenantId>&) {
    return;
}

Status ReadHedgingModeParameter::set(const BSONElement& newValueElement,
                                     const boost::optional<TenantId>&) {
    LOGV2_WARNING(
        9206300,
        "Hedged reads have been deprecated and the readHedgingMode parameter has no effect. "
        "For more information please see "
        "https://dochub.mongodb.org/core/hedged-reads-deprecated");

    return Status::OK();
}

Status ReadHedgingModeParameter::setFromString(std::string_view modeStr,
                                               const boost::optional<TenantId>&) {
    LOGV2_WARNING(
        9206301,
        "Hedged reads have been deprecated and the readHedgingMode parameter has no effect. "
        "For more information please see "
        "https://dochub.mongodb.org/core/hedged-reads-deprecated");

    return Status::OK();
}

void MaxTimeMSForHedgedReadsParameter::append(OperationContext*,
                                              BSONObjBuilder* builder,
                                              std::string_view name,
                                              const boost::optional<TenantId>&) {
    return;
}

Status MaxTimeMSForHedgedReadsParameter::set(const BSONElement& newValueElement,
                                             const boost::optional<TenantId>&) {
    LOGV2_WARNING(9206302,
                  "Hedged reads have been deprecated and the maxTimeMSForHedgedReads parameter has "
                  "no effect. "
                  "For more information please see "
                  "https://dochub.mongodb.org/core/hedged-reads-deprecated");

    return Status::OK();
}

Status MaxTimeMSForHedgedReadsParameter::setFromString(std::string_view modeStr,
                                                       const boost::optional<TenantId>&) {
    LOGV2_WARNING(9206303,
                  "Hedged reads have been deprecated and the maxTimeMSForHedgedReads parameter has "
                  "no effect. "
                  "For more information please see "
                  "https://dochub.mongodb.org/core/hedged-reads-deprecated");

    return Status::OK();
}

}  // namespace mongo
