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

#include "mongo/db/shard_role/shard_catalog/collection_uuid_mismatch_info.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/database_name_util.h"
#include "mongo/db/tenant_id.h"

#include <string_view>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/type_traits/decay.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;
MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(CollectionUUIDMismatchInfo);

constexpr std::string_view kDbFieldName = "db"sv;
constexpr std::string_view kCollectionUUIDFieldName = "collectionUUID"sv;
constexpr std::string_view kExpectedCollectionFieldName = "expectedCollection"sv;
constexpr std::string_view kActualCollectionFieldName = "actualCollection"sv;
}  // namespace

std::shared_ptr<const ErrorExtraInfo> CollectionUUIDMismatchInfo::parse(const BSONObj& obj) {
    auto actualNamespace = obj[kActualCollectionFieldName];
    return std::make_shared<CollectionUUIDMismatchInfo>(
        // Deserialize db name object from a string which is formated for error messages.
        DatabaseNameUtil::deserializeForErrorMsg(obj[kDbFieldName].str()),
        UUID::parse(obj[kCollectionUUIDFieldName]).getValue(),
        obj[kExpectedCollectionFieldName].str(),
        actualNamespace.isNull() ? boost::none : boost::make_optional(actualNamespace.str()));
}

void CollectionUUIDMismatchInfo::serialize(BSONObjBuilder* builder) const {
    // Serialize to the extra error message of a an error Status.
    builder->append(kDbFieldName, _dbName.toStringForErrorMsg());
    _collectionUUID.appendToBuilder(builder, kCollectionUUIDFieldName);
    builder->append(kExpectedCollectionFieldName, _expectedCollection);
    if (_actualCollection) {
        builder->append(kActualCollectionFieldName, *_actualCollection);
    } else {
        builder->appendNull(kActualCollectionFieldName);
    }
}
}  // namespace mongo
