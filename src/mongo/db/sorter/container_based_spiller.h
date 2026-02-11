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
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/modules.h"

#include <limits>
#include <memory>
#include <span>
#include <utility>

#include <boost/optional.hpp>

MONGO_MOD_PUB;
namespace mongo::sorter {

// TODO SERVER-119549 Make container buffer size more accurate
constexpr inline std::size_t kContainerBufferSize = size_t{64} << 10;

template <typename Key, typename Value>
class ContainerIterator : public Iterator<Key, Value> {
public:
    /**
     * Constructs an iterator using the given cursor, from `start` (inclusive) up to `end`
     * (exclusive).
     */
    ContainerIterator(std::unique_ptr<IntegerKeyedContainer::Cursor> cursor,
                      int64_t start,
                      int64_t end,
                      Iterator<Key, Value>::Settings settings,
                      const size_t checksum,
                      const SorterChecksumVersion checksumVersion)
        : _cursor(std::move(cursor)),
          _start(start),
          _position(_unpositioned),
          _end(end),
          _settings(std::move(settings)),
          _checksumCalculator(checksumVersion),
          _originalChecksum(checksum) {}

    bool more() override {
        return _position < _end - 1;
    }

    std::pair<Key, Value> next() override {
        auto result = _next();
        BufReader reader{result.data(), static_cast<unsigned>(result.size())};
        _checksumCalculator.addData(result.data(), result.size());
        _compareChecksums();

        return {Key::deserializeForSorter(reader, _settings.first),
                Value::deserializeForSorter(reader, _settings.second)};
    }

    Key nextWithDeferredValue() override {
        uassert(
            10896301, "Must follow nextWithDeferredValue with getDeferredValue", !_deferredValue);

        auto result = _next();
        BufReader reader{result.data(), static_cast<unsigned>(result.size())};
        _checksumCalculator.addData(result.data(), result.size());
        _compareChecksums();

        auto key = Key::deserializeForSorter(reader, _settings.first);
        _deferredValue.emplace(result.data() + reader.offset(), result.size() - reader.offset());

        return std::move(key);
    }

    Value getDeferredValue() override {
        uassert(
            10896303, "Must precede getDeferredValue with nextWithDeferredValue", _deferredValue);

        BufReader reader{_deferredValue->data(), static_cast<unsigned>(_deferredValue->size())};
        _deferredValue = boost::none;

        return Value::deserializeForSorter(reader, _settings.second);
    }

    const Key& peek() override {
        MONGO_UNREACHABLE_TASSERT(10896305);
    }

    SorterRange getRange() const override {
        return {_start, _end, static_cast<int64_t>(_originalChecksum)};
    }

    bool spillable() const override {
        return false;
    }

    std::unique_ptr<Iterator<Key, Value>> spill(
        const SortOptions& opts, const typename Sorter<Key, Value>::Settings& settings) override {
        MONGO_UNREACHABLE_TASSERT(10896307);
    }

private:
    static constexpr int64_t _unpositioned = -1;

    std::span<const char> _next() {
        if (_position == _unpositioned) {
            auto result = _cursor->find(_start);
            uassert(10896300,
                    fmt::format("Sorter container unexpectedly missing key {}", _position),
                    result);
            _position = _start;
            return *result;
        }

        auto result = _cursor->next();
        uassert(11786000,
                fmt::format("Sorter container unexpectedly reached end before key {}", _position),
                result);
        uassert(11786001,
                fmt::format("Sorter container unexpectedly got key {} instead of {}",
                            result->first,
                            _position),
                result->first == ++_position);
        return result->second;
    }

    void _compareChecksums() {
        if (!more() && _originalChecksum != _checksumCalculator.checksum()) {
            fassert(11605900,
                    Status(ErrorCodes::Error::ChecksumMismatch,
                           "Data read from container does not match what was written to container. "
                           "Possible corruption of data."));
        }
    }

    std::unique_ptr<IntegerKeyedContainer::Cursor> _cursor;
    int64_t _start;
    int64_t _position;
    int64_t _end;
    Iterator<Key, Value>::Settings _settings;
    boost::optional<std::span<const char>> _deferredValue;

    // Checksum value that is updated with each read of a data object from a container. We can
    // compare this value with _originalChecksum to check for data corruption if and only if the
    // ContainerIterator is exhausted.
    SorterChecksumCalculator _checksumCalculator;

    // Checksum value retrieved from SortedContainerWriter that was calculated as data was spilled
    // to disk. This is not modified, and is only used for comparison against _afterReadChecksum
    // when the ContainerIterator is exhausted to ensure no data corruption.
    const size_t _originalChecksum;
};

/**
 * Appends a pre-sorted range of data to a container and hands back an Iterator over the range.
 */
template <typename Key, typename Value>
class SortedContainerWriter final : public SortedStorageWriter<Key, Value> {
public:
    typedef sorter::Iterator<Key, Value> Iterator;
    using Settings = SortedStorageWriter<Key, Value>::Settings;

    SortedContainerWriter(OperationContext& opCtx,
                          RecoveryUnit& ru,
                          const CollectionPtr& collection,
                          IntegerKeyedContainer& container,
                          SorterContainerStats& containerStats,
                          const SortOptions& opts,
                          int64_t nextKey,
                          const Settings& settings)
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
        return std::make_shared<ContainerIterator<Key, Value>>(_container.getCursor(_ru),
                                                               _rangeStartKey,
                                                               _nextKey,
                                                               this->_settings,
                                                               this->_checksumCalculator.checksum(),
                                                               this->_checksumCalculator.version());
    }
    std::unique_ptr<Iterator> doneUnique() override {
        return std::make_unique<ContainerIterator<Key, Value>>(_container.getCursor(_ru),
                                                               _rangeStartKey,
                                                               _nextKey,
                                                               this->_settings,
                                                               this->_checksumCalculator.checksum(),
                                                               this->_checksumCalculator.version());
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

template <typename Key, typename Value>
class ContainerBasedSorterStorage : public SorterStorageBase<Key, Value> {
public:
    using Settings = SorterStorageBase<Key, Value>::Settings;

    ContainerBasedSorterStorage(OperationContext& opCtx,
                                RecoveryUnit& ru,
                                const CollectionPtr& collection,
                                IntegerKeyedContainer& container,
                                SorterContainerStats& stats,
                                int64_t currKey,
                                boost::optional<DatabaseName> dbName = boost::none,
                                SorterChecksumVersion checksumVersion = SorterChecksumVersion::v2)
        : SorterStorageBase<Key, Value>(std::move(dbName), checksumVersion),
          _opCtx(opCtx),
          _ru(ru),
          _collection(collection),
          _container(container),
          _stats(stats),
          _currKey(currKey) {}

    std::unique_ptr<SortedStorageWriter<Key, Value>> makeWriter(const SortOptions& opts,
                                                                const Settings& settings) override {
        return std::make_unique<sorter::SortedContainerWriter<Key, Value>>(
            _opCtx, _ru, _collection, _container, _stats, opts, _currKey, settings);
    };

    size_t getIteratorSize() override {
        return sizeof(sorter::ContainerIterator<Key, Value>);
    };

    // TODO SERVER-119549 Make container buffer size more accurate
    size_t getBufferSize() override {
        return kContainerBufferSize;
    }

    std::shared_ptr<sorter::Iterator<Key, Value>> getSortedIterator(
        const SorterRange& range, const Settings& settings) override {
        MONGO_UNIMPLEMENTED_TASSERT(11374700);
    };

    std::string getStorageIdentifier() override {
        MONGO_UNIMPLEMENTED_TASSERT(11374701);
    };

    void keep() override {
        MONGO_UNIMPLEMENTED_TASSERT(11374702);
    };

    boost::optional<boost::filesystem::path> getSpillDirPath() override {
        auto ident = _container.ident();
        invariant(ident);
        auto dir = ident::getDirectory(ident->getIdent());
        boost::filesystem::path path{storageGlobalParams.dbpath};
        if (!dir.empty()) {
            path /= std::string{dir};
        }
        return path;
    };

    /**
     * Updates the key assigned for a KV pair for SortedContainerWriter creation.
     */
    void updateCurrKey(int64_t newKey) {
        _currKey = newKey;
    }

    void remove(int64_t key) {
        WriteUnitOfWork wuow{&_opCtx};
        uassertStatusOK(container_write::remove(&_opCtx, _ru, _collection, _container, key));
        wuow.commit();
    }

private:
    OperationContext& _opCtx;
    RecoveryUnit& _ru;
    const CollectionPtr& _collection;
    IntegerKeyedContainer& _container;
    SorterContainerStats& _stats;
    int64_t _currKey;
};

template <typename Key, typename Value>
class ContainerBasedSpiller : public SorterSpillerBase<Key, Value> {
public:
    ContainerBasedSpiller(OperationContext& opCtx,
                          RecoveryUnit& ru,
                          const CollectionPtr& collection,
                          IntegerKeyedContainer& container,
                          SorterContainerStats& stats,
                          boost::optional<DatabaseName> dbName,
                          SorterChecksumVersion checksumVersion,
                          int64_t batchSize)
        : SorterSpillerBase<Key, Value>(std::make_unique<ContainerBasedSorterStorage<Key, Value>>(
              opCtx, ru, collection, container, stats, 1, std::move(dbName), checksumVersion)),
          _opCtx(opCtx),
          _batchSize(batchSize) {}

    void mergeSpills(const SortOptions& opts,
                     const SorterSpillerBase<Key, Value>::Settings& settings,
                     SorterStats& stats,
                     std::vector<std::shared_ptr<sorter::Iterator<Key, Value>>>& iters,
                     SorterSpillerBase<Key, Value>::Comparator comp,
                     std::size_t numTargetedSpills,
                     std::size_t numParallelSpills) override {
        std::vector<std::shared_ptr<sorter::Iterator<Key, Value>>> oldIters;
        while (iters.size() > numTargetedSpills) {
            oldIters.swap(iters);
            for (size_t i = 0; i < oldIters.size(); i += numParallelSpills) {
                auto count = std::min(numParallelSpills, oldIters.size() - i);
                auto spillsToMerge = std::span(oldIters).subspan(i, count);
                auto mergeIterator = sorter::merge<Key, Value>(spillsToMerge, opts, comp);
                auto writer = this->_storage->makeWriter(opts, settings);

                int64_t start = std::numeric_limits<int64_t>::max();
                int64_t end = 0;
                int64_t numSpilled = 0;

                while (mergeIterator->more()) {
                    auto range = mergeIterator->iterator().getRange();
                    if (range.getStartOffset() < start) {
                        start = range.getStartOffset();
                    }
                    if (range.getEndOffset() > end) {
                        end = range.getEndOffset();
                    }

                    auto next = mergeIterator->next();
                    writer->addAlreadySorted(next.first, next.second);
                    ++numSpilled;
                }
                invariant(numSpilled == end - start);

                // TOOD (SERVER-117546): Use a truncate rather than individual deletes.
                for (int64_t current = start; current < end; ++current) {
                    _containerBasedStorage().remove(current);
                }

                iters.push_back(writer->done());
                _current += numSpilled;
                _containerBasedStorage().updateCurrKey(_current);

                stats.incrementSpilledRanges();
                stats.incrementSpilledKeyValuePairs(numSpilled);
            }
            oldIters.clear();
        }
    }

private:
    ContainerBasedSorterStorage<Key, Value>& _containerBasedStorage() {
        return *static_cast<ContainerBasedSorterStorage<Key, Value>*>(this->_storage.get());
    }

    std::unique_ptr<SortedStorageWriter<Key, Value>> _spill(
        const SortOptions& opts,
        const SorterSpillerBase<Key, Value>::Settings& settings,
        std::span<std::pair<Key, Value>> data,
        uint32_t idx) override {
        auto writer = this->_storage->makeWriter(opts, settings);
        int64_t numAdded = 0;
        boost::optional<WriteUnitOfWork> wuow{boost::in_place_init, &_opCtx};
        for (auto&& [key, value] : data.subspan(idx)) {
            writer->addAlreadySorted(key, value);
            ++numAdded;
            ++_current;
            if (numAdded % _batchSize == 0) {
                wuow->commit();
                wuow.emplace(&_opCtx);
            }
        }
        wuow->commit();
        _containerBasedStorage().updateCurrKey(_current);
        return std::move(writer);
    }

    OperationContext& _opCtx;
    int64_t _batchSize;
    int64_t _current = 1;
};

}  // namespace mongo::sorter
