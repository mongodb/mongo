// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/would_change_owning_shard_exception.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"

#include <string_view>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(WouldChangeOwningShardInfo);

constexpr std::string_view kPreImage = "preImage"sv;
constexpr std::string_view kPostImage = "postImage"sv;
constexpr std::string_view kShouldUpsert = "shouldUpsert"sv;
constexpr std::string_view kUserPostImage = "userPostImage"sv;

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
