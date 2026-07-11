// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/shard_filterer.h"
#include "mongo/util/modules.h"

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

    size_t getApproximateSize() const override {
        auto size = sizeof(ConstantFilterMock);
        size += _pattern.getApproximateSize() - sizeof(KeyPattern);
        return size;
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

    size_t getApproximateSize() const override {
        auto size = sizeof(AllNullShardKeyFilterMock);
        size += _pattern.getApproximateSize() - sizeof(ShardKeyPattern);
        return size;
    }

private:
    const ShardKeyPattern _pattern;
};
}  // namespace mongo
