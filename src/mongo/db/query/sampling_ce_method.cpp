// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_integration_knobs_gen.h"
#include "mongo/db/query/query_optimization_knobs_gen.h"
#include "mongo/db/tenant_id.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/synchronized_value.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {

void CBRSamplingCEMethod::append(OperationContext*,
                                 BSONObjBuilder* b,
                                 std::string_view name,
                                 const boost::optional<TenantId>&) {
    *b << name << idl::serialize(_data.get());
}

Status CBRSamplingCEMethod::setFromString(std::string_view value,
                                          const boost::optional<TenantId>&) {
    _data = idl::deserialize<SamplingCEMethodEnum>(
        value, IDLParserContext("internalQuerySamplingCEMethod"));
    return Status::OK();
}

void PersistentSampleCEMethod::append(OperationContext*,
                                      BSONObjBuilder* b,
                                      std::string_view name,
                                      const boost::optional<TenantId>&) {
    *b << name << idl::serialize(_data.get());
}

Status PersistentSampleCEMethod::setFromString(std::string_view value,
                                               const boost::optional<TenantId>&) {
    _data = idl::deserialize<SamplingCEMethodEnum>(
        value, IDLParserContext("internalQuerySamplingCEMethodForPersistentSamples"));
    return Status::OK();
}

void JoinSamplingCEMethod::append(OperationContext*,
                                  BSONObjBuilder* b,
                                  std::string_view name,
                                  const boost::optional<TenantId>&) {
    *b << name << idl::serialize(_data.get());
}

Status JoinSamplingCEMethod::setFromString(std::string_view value,
                                           const boost::optional<TenantId>&) {
    _data = idl::deserialize<SamplingCEMethodEnum>(
        value, IDLParserContext("internalJoinOptimizationSamplingCEMethod"));
    return Status::OK();
}

}  // namespace mongo
