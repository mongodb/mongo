// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/spilling/spilling_stats.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/spill_table.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <deque>
#include <memory>
#include <string_view>

namespace mongo {

class SpillableDocumentMapImpl {
public:
    SpillableDocumentMapImpl(ExpressionContext* expCtx,
                             MemoryUsageTracker* tracker,
                             std::string_view memoryTrackerName = "SpillableDocumentMapImpl")
        : _memTracker((*tracker)[memoryTrackerName]), _expCtx(expCtx) {}

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
        return _memTracker.inUseTrackedMemoryBytes();
    }

    size_t size() const {
        return _diskMapSize + _memMap.size();
    }

    bool empty() const {
        return size() == 0;
    }

    bool usedDisk() const {
        return _stats.getSpills() > 0;
    }

    /**
     * Spills all values currently in memory to disk. Throws if '_expCtx->getAllowDiskUse()' is
     * false. This is also called automatically as part of 'add', but call this to spill
     * without adding.
     *
     * Invalidates iterators.
     */
    void spillToDisk();

    bool hasInMemoryData() const {
        return !_memMap.empty();
    }

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
        void releaseMemory();

    private:
        using MapPointer =
            std::conditional_t<IsConst, const SpillableDocumentMapImpl*, SpillableDocumentMapImpl*>;
        using MemIterator = std::conditional_t<
            IsConst,
            ValueFlatUnorderedMap<MemoryUsageTokenWith<Document>>::const_iterator,
            ValueFlatUnorderedMap<MemoryUsageTokenWith<Document>>::iterator>;

        friend SpillableDocumentMapImpl;
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

        std::unique_ptr<SpillTable::Cursor> _diskIt = nullptr;
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

    /**
     * If the element that it is pointing to is stored in memory, removes it.
     * Advances the iterator to the next element. Illegal to call on end().
     */
    void eraseIfInMemoryAndAdvance(Iterator& it) {
        if (it.memoryExhausted()) {
            ++it;
        } else {
            auto itToErase = it._memIt;
            ++it;
            _memMap.erase(itToErase);
        }
    }

protected:
    void _add(Value id, Document document, size_t size);

private:
    void initDiskMap();
    RecordId computeKey(const Value& id) const;

    void updateStorageSizeStat();

    // The pipeline stage using this class owns this memory tracker; this class only references it.
    SimpleMemoryUsageTracker& _memTracker;
    ExpressionContext* _expCtx;

    ValueFlatUnorderedMap<MemoryUsageTokenWith<Document>> _memMap =
        ValueComparator::kInstance.makeFlatUnorderedValueMap<MemoryUsageTokenWith<Document>>();

    std::unique_ptr<SpillTable> _diskMap = nullptr;
    size_t _diskMapSize = 0;

    mutable key_string::Builder _builder{key_string::Version::kLatestVersion};

    SpillingStats _stats;
};


/**
 * A hash map for documents that uses _id as the key that spills to disk if the contents of the map
 * are larger than the given maximum memory. Contents on disk are in a temporary table that will be
 * cleaned up on startup if the server crashes with data still present.
 *
 * Does not support collation.
 */
class SpillableDocumentMap : public SpillableDocumentMapImpl {
public:
    using SpillableDocumentMapImpl::SpillableDocumentMapImpl;

    void add(Document document);
};

/**
 * A hash set for values that spills to disk if the contents of the map
 * are larger than the given maximum memory. Contents on disk are in a temporary table that will be
 * cleaned up on startup if the server crashes with data still present.
 *
 * Does not support collation.
 */
class SpillableValueSet : public SpillableDocumentMapImpl {
public:
    using SpillableDocumentMapImpl::SpillableDocumentMapImpl;

    void add(Value id);
};

}  // namespace mongo
