/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/collection_crud/container_write.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/sorter/sorter.h"
#include "mongo/db/storage/container.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/modules.h"

MONGO_MOD_PUB;
namespace mongo::sorter {

/**
 * Appends a pre-sorted range of data to a container and hands back an Iterator over the range.
 */
template <typename Key, typename Value>
class SortedContainerWriter final : public SortedStorageWriter<Key, Value> {
public:
    typedef sorter::Iterator<Key, Value> Iterator;
    typedef std::pair<typename Key::SorterDeserializeSettings,
                      typename Value::SorterDeserializeSettings>
        Settings;

    SortedContainerWriter(OperationContext& opCtx,
                          RecoveryUnit& ru,
                          const CollectionPtr& collection,
                          IntegerKeyedContainer& container,
                          SorterContainerStats& containerStats,
                          const SortOptions& opts,
                          int64_t nextKey,
                          const Settings& settings = Settings())
        : SortedStorageWriter<Key, Value>(opts, settings),
          _opCtx(opCtx),
          _ru(ru),
          _collection(collection),
          _container(container),
          _containerStats(containerStats),
          _nextKey(nextKey),
          _rangeStartKey(nextKey) {}

    /*
     * Serializes a single KV pair and inserts it into the container within its own write unit of
     * work, advancing the container key range.
     */
    void addAlreadySorted(const Key& key, const Value& val) override {
        BufBuilder buffer;
        key.serializeForSorter(buffer);
        val.serializeForSorter(buffer);

        const auto currentKey = _nextKey++;
        const auto size = static_cast<size_t>(buffer.len());
        const std::span<const char> value =
            size == 0 ? std::span<const char>{} : std::span<const char>(buffer.buf(), size);

        WriteUnitOfWork wuow(&_opCtx);
        uassertStatusOK(
            container_write::insert(&_opCtx, _ru, _collection, _container, currentKey, value));
        wuow.commit();

        if (size > 0) {
            this->_checksumCalculator.addData(buffer.buf(), size);
        }
        _containerStats.addSpilledDataSizeUncompressed(size);
    }

    std::shared_ptr<Iterator> done() override {
        // TODO SERVER-108963: Construct a ContainerIterator.
        MONGO_UNIMPLEMENTED_TASSERT(10895401);
        return {};
    }
    std::unique_ptr<Iterator> doneUnique() override {
        // TODO SERVER-108963: Construct a ContainerIterator.
        MONGO_UNIMPLEMENTED_TASSERT(10895402);
        return {};
    }

    void writeChunk() override {};

private:
    OperationContext& _opCtx;
    RecoveryUnit& _ru;
    const CollectionPtr& _collection;
    IntegerKeyedContainer& _container;
    SorterContainerStats& _containerStats;
    int64_t _nextKey;
    int64_t _rangeStartKey;
};

}  // namespace mongo::sorter
