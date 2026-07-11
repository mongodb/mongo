// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
