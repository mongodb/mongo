/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/exec/shard_filterer.h"

namespace mongo {

/**
 * A mock that implements a constant ShardFilterer suitable for unittesting shard filtering.
 */
class ConstantFilterMock final : public ShardFilterer {
public:
    ConstantFilterMock(bool pass, const BSONObj& pattern)
        : _pass(pass), _pattern(pattern.getOwned()) {}

    std::unique_ptr<ShardFilterer> clone() const override {
        return std::make_unique<ConstantFilterMock>(_pass, _pattern.toBSON());
    }

    bool keyBelongsToMe(const BSONObj& key) const override {
        return _pass;
    }

    DocumentBelongsResult documentBelongsToMe(const BSONObj& doc) const override {
        return keyBelongsToMe(BSONObj{}) ? DocumentBelongsResult::kBelongs
                                         : DocumentBelongsResult::kDoesNotBelong;
    }

    const KeyPattern& getKeyPattern() const override {
        return _pattern;
    }

    bool isCollectionSharded() const override {
        return true;
    }

private:
    const bool _pass;
    const KeyPattern _pattern;
};

/**
 * A mock ShardFilterer that filters out documents that contain nulls in all shard key fields.
 */
class AllNullShardKeyFilterMock : public ShardFilterer {
public:
    explicit AllNullShardKeyFilterMock(const BSONObj& pattern) : _pattern(pattern.getOwned()) {}

    std::unique_ptr<ShardFilterer> clone() const override {
        return std::make_unique<AllNullShardKeyFilterMock>(_pattern.toBSON());
    }

    bool keyBelongsToMe(const BSONObj& key) const override {
        for (auto&& elem : key) {
            if (!elem.isNull()) {
                return true;
            }
        }
        return false;
    }

    DocumentBelongsResult documentBelongsToMe(const BSONObj& doc) const override {
        auto shardKey = _pattern.extractShardKeyFromDoc(doc);
        if (shardKey.isEmpty()) {
            return DocumentBelongsResult::kNoShardKey;
        }
        return keyBelongsToMe(shardKey) ? DocumentBelongsResult::kBelongs
                                        : DocumentBelongsResult::kDoesNotBelong;
    }

    const KeyPattern& getKeyPattern() const override {
        return _pattern.getKeyPattern();
    }

    bool isCollectionSharded() const override {
        return true;
    }

private:
    const ShardKeyPattern _pattern;
};
}  // namespace mongo
