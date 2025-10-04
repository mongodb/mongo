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

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

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

    explicit WouldChangeOwningShardInfo(const BSONObj& preImage,
                                        const BSONObj& postImage,
                                        const bool shouldUpsert,
                                        boost::optional<NamespaceString> ns,
                                        boost::optional<UUID> uuid,
                                        boost::optional<BSONObj> userPostImage = boost::none)
        : _preImage(preImage.getOwned()),
          _postImage(postImage.getOwned()),
          _shouldUpsert(shouldUpsert),
          _ns(ns),
          _uuid(uuid),
          _userPostImage(userPostImage) {}

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
};
using WouldChangeOwningShardException = ExceptionFor<ErrorCodes::WouldChangeOwningShard>;

}  // namespace mongo
