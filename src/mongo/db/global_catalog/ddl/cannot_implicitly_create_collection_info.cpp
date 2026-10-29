// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/cannot_implicitly_create_collection_info.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string_util.h"

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(CannotImplicitlyCreateCollectionInfo);

}  // namespace

void CannotImplicitlyCreateCollectionInfo::serialize(BSONObjBuilder* bob) const {
    bob->append("ns", _nss.toStringForErrorMsg());
}

std::shared_ptr<const ErrorExtraInfo> CannotImplicitlyCreateCollectionInfo::parse(
    const BSONObj& obj) {
    return std::make_shared<CannotImplicitlyCreateCollectionInfo>(
        NamespaceStringUtil::deserializeForErrorMsg(obj["ns"].str()));
}

}  // namespace mongo
