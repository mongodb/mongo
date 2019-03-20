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

#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

/**
 * This error is thrown when an update would cause a document to be owned by a different
 * shard. If the update is part of a multi statement transaction, we will attach the
 * pre image and the post image returned by the update stage. MongoS will use these to delete
 * the original doc and insert the new doc. If the update is a retryable write, we will attach
 * only the pre image.
 */
class WouldChangeOwningShardInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::WouldChangeOwningShard;

    explicit WouldChangeOwningShardInfo(const BSONObj& preImage,
                                        const boost::optional<BSONObj>& postImage)
        : _preImage(preImage.getOwned()) {
        if (postImage)
            _postImage = postImage->getOwned();
    }

    const auto& getPreImage() const {
        return _preImage;
    }

    const auto& getPostImage() const {
        return _postImage;
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
    boost::optional<BSONObj> _postImage;
};
using WouldChangeOwningShardException = ExceptionFor<ErrorCodes::WouldChangeOwningShard>;

}  // namespace mongo
