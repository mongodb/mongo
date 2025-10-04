/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/s/would_change_owning_shard_exception.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(WouldChangeOwningShardInfo);

constexpr StringData kPreImage = "preImage"_sd;
constexpr StringData kPostImage = "postImage"_sd;
constexpr StringData kShouldUpsert = "shouldUpsert"_sd;
constexpr StringData kUserPostImage = "userPostImage"_sd;

}  // namespace

void WouldChangeOwningShardInfo::serialize(BSONObjBuilder* bob) const {
    bob->append(kPreImage, _preImage);
    bob->append(kPostImage, _postImage);
    bob->append(kShouldUpsert, _shouldUpsert);
    if (_userPostImage) {
        bob->append(kUserPostImage, *_userPostImage);
    }
}

std::shared_ptr<const ErrorExtraInfo> WouldChangeOwningShardInfo::parse(const BSONObj& obj) {
    return std::make_shared<WouldChangeOwningShardInfo>(parseFromCommandError(obj));
}

WouldChangeOwningShardInfo WouldChangeOwningShardInfo::parseFromCommandError(const BSONObj& obj) {
    return WouldChangeOwningShardInfo(
        obj[kPreImage].Obj().getOwned(),
        obj[kPostImage].Obj().getOwned(),
        obj[kShouldUpsert].Bool(),
        boost::none,
        boost::none,
        obj.hasField(kUserPostImage) ? boost::make_optional(obj[kUserPostImage].Obj().getOwned())
                                     : boost::none);
}

}  // namespace mongo
