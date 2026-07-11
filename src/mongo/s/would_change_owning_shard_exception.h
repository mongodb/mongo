// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

// TODO SERVER-113081 Investigate whether this can be made private to the query module.
namespace [[MONGO_MOD_NEEDS_REPLACEMENT]] mongo {

/**
 * This error is thrown when an update would cause a document to be owned by a different
 * shard. If the update is part of a multi statement transaction, we will attach the
 * pre image and the post image returned by the update stage. MongoS will use these to delete
 * the original doc and insert the new doc. If the update is a retryable write, we will attach
 * only the pre image. If the update is an insertion, caused by an upsert, an original predicate
 * will be passed as the pre image.
 */
class WouldChangeOwningShardInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::WouldChangeOwningShard;

    explicit WouldChangeOwningShardInfo(
        const BSONObj& preImage,
        const BSONObj& postImage,
        const bool shouldUpsert,
        boost::optional<NamespaceString> ns,
        boost::optional<UUID> uuid,
        boost::optional<BSONObj> userPostImage = boost::none,
        boost::optional<ShardId> preImageReshardingDestinedShard = boost::none)
        : _preImage(preImage.getOwned()),
          _postImage(postImage.getOwned()),
          _shouldUpsert(shouldUpsert),
          _ns(ns),
          _uuid(uuid),
          _userPostImage(userPostImage),
          _preImageReshardingDestinedShard(preImageReshardingDestinedShard) {}

    const auto& getPreImage() const {
        return _preImage;
    }

    const auto& getPostImage() const {
        return _postImage;
    }

    const auto& getShouldUpsert() const {
        return _shouldUpsert;
    }

    const auto& getNs() const {
        return _ns;
    }

    const auto& getUuid() const {
        return _uuid;
    }

    const boost::optional<BSONObj>& getUserPostImage() const {
        return _userPostImage;
    }

    BSONObj toBSON() const {
        BSONObjBuilder bob;
        serialize(&bob);
        return bob.obj();
    }

    boost::optional<ShardId> getPreImageReshardingDestinedShard() const {
        return _preImageReshardingDestinedShard;
    }

    void serialize(BSONObjBuilder* bob) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);
    static WouldChangeOwningShardInfo parseFromCommandError(const BSONObj& commandError);

private:
    // The pre image of the document
    BSONObj _preImage;

    // The post image returned by the update stage
    BSONObj _postImage;

    // True if {upsert: true} and the update stage did not match any docs
    bool _shouldUpsert;

    // The namespace of the collection containing the document. Does not get serialized into the
    // BSONObj for this error.
    boost::optional<NamespaceString> _ns;

    // The uuid of collection containing the document. Does not get serialized into the BSONObj for
    // this error.
    boost::optional<UUID> _uuid;

    // The user-level post image for shard key update on a sharded timeseries collection.
    boost::optional<BSONObj> _userPostImage;

    // The destined shard in case the update affects the post-resharding distribution. Only
    // present in case the collection is subject to resharding. Does not get serialized into the
    // BSONObj for this error as it's only useful to forward information on the same shard
    // stack.
    boost::optional<ShardId> _preImageReshardingDestinedShard;
};
using WouldChangeOwningShardException = ExceptionFor<ErrorCodes::WouldChangeOwningShard>;

}  // namespace mongo
