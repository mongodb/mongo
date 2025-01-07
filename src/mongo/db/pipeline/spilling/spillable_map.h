/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include <cstddef>
#include <deque>
#include <memory>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/spilling/spilling_stats.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/temporary_record_store.h"
#include "mongo/util/memory_usage_tracker.h"

namespace mongo {

/**
 * A hash map for documents that uses _id as the key that spills to disk if the contents of the map
 * are larger than the given maximum memory. Contents on disk are in a temporary table that will be
 * cleaned up on startup if the server crashes with data still present.
 *
 * Does not support collation.
 */
class SpillableDocumentMap {
public:
    SpillableDocumentMap(ExpressionContext* expCtx, MemoryUsageTracker* tracker)
        : _memTracker((*tracker)["SpillableValueDocumentHashMap"]), _expCtx(expCtx) {}

    /**
     * Adds 'document' to the map using _id field as key and spills to disk if the size puts us over
     * the memory limit and spilling is allowed.
     *
     * Inserting multiple documents with the same _id is not supported.
     *
     * Invalidates iterators.
     */
    void add(Document document);

    bool contains(const Value& id) const;

    /**
     * Removes all values from the map and reset state while preserving the ability to perform
     * more inserts.
     */
    void clear();

    /**
     * Releases allocated memory and disk resources.
     */
    void dispose();

    size_t getApproximateSize() const {
        return _memTracker.currentMemoryBytes();
    }

    size_t size() const {
        return _diskMapSize + _memMap.size();
    }

    bool empty() const {
        return size() == 0;
    }

    bool usedDisk() const {
        return _stats.spills > 0;
    }

    /**
     * Spills all values currently in memory to disk. Throws if '_expCtx->getAllowDiskUse()' is
     * false. This is also called automatically as part of 'add', but call this to spill
     * without adding.
     *
     * Invalidates iterators.
     */
    void spillToDisk();

    const SpillingStats& getSpillingStats() const {
        return _stats;
    }

    template <bool IsConst>
    class IteratorImpl {
    public:
        using difference_type = std::ptrdiff_t;
        using value_type = Document;
        using reference_type = std::conditional_t<IsConst, const Document&, Document&>;
        using pointer_type = std::conditional_t<IsConst, const Document*, Document*>;
        using iterator_category = std::input_iterator_tag;

        IteratorImpl(const IteratorImpl&) = delete;
        IteratorImpl& operator=(const IteratorImpl&) = delete;

        IteratorImpl(IteratorImpl&&) = default;
        IteratorImpl& operator=(IteratorImpl&&) = default;

        bool operator==(const IteratorImpl&) const;
        bool operator!=(const IteratorImpl& rhs) const {
            return !((*this) == rhs);
        }

        IteratorImpl& operator++();

        reference_type operator*() {
            return getCurrentDocument();
        }
        pointer_type operator->() {
            return &getCurrentDocument();
        }

        /**
         * If reading from disk, resets disk cursor to the first document from the in-memory buffer
         * and removes all other buffered documents.
         */
        void spill();

    private:
        using MapPointer =
            std::conditional_t<IsConst, const SpillableDocumentMap*, SpillableDocumentMap*>;
        using MemIterator = std::conditional_t<
            IsConst,
            ValueFlatUnorderedMap<MemoryUsageTokenWith<Document>>::const_iterator,
            ValueFlatUnorderedMap<MemoryUsageTokenWith<Document>>::iterator>;

        friend SpillableDocumentMap;
        struct EndTag {};

        IteratorImpl(MapPointer map);
        IteratorImpl(MapPointer map, const EndTag&);

        bool memoryExhausted() const;
        bool diskExhausted() const;

        void readNextBatchFromDisk();

        reference_type getCurrentDocument();

        void restoreDiskIt();
        void saveDiskIt();

        MapPointer _map;
        MemIterator _memIt;

        std::unique_ptr<SeekableRecordCursor> _diskIt = nullptr;
        bool _diskItExhausted = true;
        std::deque<MemoryUsageTokenWith<Document>> _diskDocuments;
    };

    using Iterator = IteratorImpl<false /*IsConst*/>;
    using ConstIterator = IteratorImpl<true /*IsConst*/>;

    Iterator begin() {
        return Iterator{this};
    }

    Iterator end() {
        return Iterator{this, Iterator::EndTag{}};
    }

    ConstIterator begin() const {
        return ConstIterator{this};
    }

    ConstIterator end() const {
        return ConstIterator{this, ConstIterator::EndTag{}};
    }

private:
    void initDiskMap();
    RecordId computeKey(const Value& id) const;

    void updateStorageSizeStat();

    mutable MemoryUsageTracker::Impl _memTracker;
    ExpressionContext* _expCtx;

    ValueFlatUnorderedMap<MemoryUsageTokenWith<Document>> _memMap =
        ValueComparator::kInstance.makeFlatUnorderedValueMap<MemoryUsageTokenWith<Document>>();

    std::unique_ptr<TemporaryRecordStore> _diskMap = nullptr;
    size_t _diskMapSize = 0;

    mutable key_string::Builder _builder{key_string::Version::kLatestVersion};

    SpillingStats _stats;
};

}  // namespace mongo
