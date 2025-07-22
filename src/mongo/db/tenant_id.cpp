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

#include "mongo/db/tenant_id.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/util/assert_util.h"

#include <fmt/format.h>

namespace mongo {

TenantId TenantId::parseFromString(StringData tenantId) {
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

void TenantId::serializeToBSON(StringData fieldName, BSONObjBuilder* builder) const {
    // Append objectid to the builder.
    builder->append(fieldName, _oid);
}

void TenantId::serializeToBSON(BSONArrayBuilder* builder) const {
    builder->append(_oid);
}

}  // namespace mongo
