// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sharding_environment/shard_ref.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/str.h"

#include <string>
#include <string_view>

namespace mongo {

std::string ShardRef::toString() const {
    return visit(OverloadedVisitor{
                     [](const ShardId& id) -> std::string { return id.toString(); },
                     [](const UUID& uuid) { return uuid.toString(); },
                 },
                 _ref);
}

ShardRef::operator ShardId() const {
    return getShardId();
}

ShardRef ShardRef::parse(const BSONElement& element) {
    if (element.type() == BSONType::string) {
        return ShardRef(element.str());
    }
    uassert(ErrorCodes::BadValue,
            str::stream() << "Expected string or UUID for ShardRef, got BSON type: "
                          << typeName(element.type()),
            element.type() == BSONType::binData && element.binDataType() == BinDataType::newUUID);
    return ShardRef(uassertStatusOK(UUID::parse(element)));
}

void ShardRef::serialize(std::string_view fieldName, BSONObjBuilder* builder) const {
    visit(OverloadedVisitor{
              [&](const ShardId& ref) { builder->append(fieldName, ref.toString()); },
              [&](const UUID& ref) { ref.appendToBuilder(builder, fieldName); },
          },
          _ref);
}

void ShardRef::serialize(BSONArrayBuilder* builder) const {
    visit(OverloadedVisitor{
              [&](const ShardId& ref) { builder->append(ref.toString()); },
              [&](const UUID& ref) { ref.appendToArrayBuilder(builder); },
          },
          _ref);
}

}  // namespace mongo
