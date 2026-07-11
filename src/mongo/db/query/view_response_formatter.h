// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"
#include "mongo/util/serialization_context.h"

#include <boost/optional/optional.hpp>

namespace mongo {
class BSONObjBuilder;

/**
 * Utility functions for converting aggregation responses into other CRUD command responses.
 */
class ViewResponseFormatter {

public:
    static const char kCountField[];
    static const char kDistinctField[];
    static const char kOkField[];

    explicit ViewResponseFormatter(BSONObj aggregationResponse);

    /**
     * Extracts the `n` field from response as if '_response' were a response from the count
     * command.
     *
     * If '_response' is not a valid cursor-based response from the aggregation command, the
     * function will fail with a uassert.
     */
    long long getCountValue(
        boost::optional<TenantId> tenantId,
        const SerializationContext& serializationCtxt = SerializationContext::stateCommandReply());

    /**
     * Appends fields to 'resultBuilder' as if '_response' were a response from the distinct
     * command.
     *
     * If '_response' is not a valid cursor-based response from the aggregation command, a non-OK
     * status is returned and 'resultBuilder' will not be modified.
     */
    Status appendAsDistinctResponse(BSONObjBuilder* resultBuilder,
                                    boost::optional<TenantId> tenantId,
                                    boost::optional<BSONObj> metrics = boost::none);

private:
    BSONObj _response;
};
}  // namespace mongo
