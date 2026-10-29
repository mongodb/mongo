// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/tenant_id.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/util/assert_util.h"

#include <string_view>

#include <fmt/format.h>

namespace mongo {

TenantId TenantId::parseFromString(std::string_view tenantId) {
    uassert(ErrorCodes::BadValue, "Failed to parse empty tenantId string.", !tenantId.empty());

    const auto res = OID::parse(tenantId);
    uassert(ErrorCodes::BadValue,
            fmt::format("Failed to parse malformatted tenantId: '{}', error: {}",
                        tenantId,
                        res.getStatus().reason()),
            res.isOK());
    return TenantId(res.getValue());
}

TenantId TenantId::parseFromBSON(const BSONElement& elem) {
    if (elem.isNull()) {
        uasserted(ErrorCodes::BadValue, "Could not deserialize TenantId from empty element");
    }

    // Expect objectid in the element for tenant.
    if (elem.type() != BSONType::oid) {
        uasserted(ErrorCodes::BadValue,
                  fmt::format("Could not deserialize TenantId with type {}", elem.type()));
    }
    return TenantId(elem.OID());
}

void TenantId::serializeToBSON(std::string_view fieldName, BSONObjBuilder* builder) const {
    // Append objectid to the builder.
    builder->append(fieldName, _oid);
}

void TenantId::serializeToBSON(BSONArrayBuilder* builder) const {
    builder->append(_oid);
}

}  // namespace mongo
