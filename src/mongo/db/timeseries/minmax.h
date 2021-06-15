/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <string>
#include <vector>

namespace mongo::timeseries {

/**
 * MinMax store of a hierarchy in a flat contigous memory structure. Optimized for fast traversal in
 * lock-step of a BSONObj with the same internal field order. It does this at the expense of insert
 * performance which should be a rare operation when adding measurements to a timeseries bucket.
 * Usually we need to traverse the MinMax structure to check if we need to update any values.
 *
 * Provides search capability when field order is not what was expected and contain a fallback to
 * map lookup when linear search does not find elements within a search threshold.
 */
class MinMaxStore {
public:
    /**
     * Element data type
     */
    enum class Type : uint8_t {
        kValue,
        kObject,
        kArray,
        kUnset,
    };
    /**
     * Buffer value for a Data of type kValue
     */
    struct Value {
        std::unique_ptr<char[]> buffer;
        int size = 0;
    };

    /**
     * Min and Max data for an Element
     */
    class Data {
    public:
        friend class MinMaxStore;

        /**
         * DataType stored by this Data
         */
        Type type() const;

        /**
         * Flag to indicate if this MinMax::Data was updated since last clear.
         */
        bool updated() const;

        /**
         * Clear update flag.
         */
        void clearUpdated();

        /**
         * Value stored by this Data. Only valid to call if type is kValue.
         */
        BSONElement value() const;
        BSONType valueType() const;

        /**
         * Sets the type of this Data.
         */
        void setObject();
        void setArray();
        void setValue(const BSONElement& elem);
        void setUnset();

    private:
        Value _value;
        Type _type = Type::kUnset;
        bool _updated = false;
    };

    class Obj;

    /**
     * Element stored in this MinMaxStore.
     */
    class Element {
    public:
        friend class MinMaxStore;
        friend class MinMaxStore::Obj;

        /**
         * Field name component
         */
        StringData fieldName() const;

        /**
         * Returns true if this Element is only used for Array storage. Array field names are not
         * used and may be claimed by an Object.
         */
        bool isArrayFieldName() const;
        void claimArrayFieldNameForObject(std::string name);

        /**
         * Min data component access
         */
        Data& min();
        const Data& min() const;

        /**
         * Max data component access
         */
        Data& max();
        const Data& max() const;

    private:
        std::string _fieldName;
        Data _min;
        Data _max;
    };

    /**
     * Internal storage type to manage iterator offsets needed to implement a tree structure in a
     * flat buffer.
     */
    struct Entry {
    public:
        // Iterator offset to the entry after the last subelement
        uint32_t _offsetEnd;
        // Iterator offset to the parent entry
        uint32_t _offsetParent;
        // Data bearing element, exposed through public interfaces
        Element _element;
        // Map for faster searches. Contain mapping from field name to iterator offset to
        // subelement. Only instantiated when we've depleted our allowed linear search depth.
        std::unique_ptr<StringMap<uint32_t>> _fieldNameToIndex;
    };
    using Entries = std::vector<Entry>;

    /**
     * Forward iterator over subelements in an Obj.
     */
    class Iterator {
    public:
        friend class MinMaxStore::Obj;

        using iterator_category = std::forward_iterator_tag;
        using difference_type = ptrdiff_t;
        using value_type = Element;
        using pointer = Element*;
        using reference = Element&;

        pointer operator->();
        reference operator*();

        Iterator& operator++();

        bool operator==(const Iterator& rhs) const;
        bool operator!=(const Iterator& rhs) const;

    private:
        Iterator(Entries::iterator pos);

        Entries::iterator _pos;
    };

    /**
     * Forward iterator over subelements in an ObjView.
     */
    class ConstIterator {
    public:
        friend class MinMaxStore::Obj;

        using iterator_category = std::forward_iterator_tag;
        using difference_type = ptrdiff_t;
        using value_type = const Element;
        using pointer = const Element*;
        using reference = const Element&;

        pointer operator->() const;
        reference operator*() const;

        ConstIterator& operator++();

        bool operator==(const ConstIterator& rhs) const;
        bool operator!=(const ConstIterator& rhs) const;

    private:
        ConstIterator(std::vector<Entry>::const_iterator pos);

        Entries::const_iterator _pos;
    };


    /**
     * Represents an 'Object' within the MinMaxStore. Analogous BSONObj for BSON. Provides
     * iteration, insertion and search capability for subelements.
     */
    class Obj {
    public:
        friend class MinMaxStore;

        /**
         * Copying an Obj copies the internal position. It is not allowed to copy to an Obj owned by
         * a different MinMaxStore.
         */
        Obj& operator=(const Obj& rhs);

        /**
         * Access to the element containing user-data such as field names and min/max values.
         */
        Element& element();
        const Element& element() const;

        /**
         * Creates an Obj to the parent of this Obj.
         */
        Obj parent() const;

        /**
         * Creates an Obj for a subelement of this Obj.
         */
        Obj object(Iterator pos) const;

        /**
         * Returns the iterator position of this Obj.
         */
        Iterator iterator() const;

        /**
         * Searches this Obj for the subelement with the provided fieldName. Performs linear search
         * from start to end with a maximum search threshold. When the maximum search threshold is
         * reached this Object will perform all subsequent searches using a map lookup.
         *
         * The returned Iterator may be outside of the range [start, last).
         * If the element is not found 'last' is returned.
         */
        Iterator search(Iterator first, Iterator last, StringData fieldName);

        /**
         * As above but last = end()
         */
        Iterator search(Iterator first, StringData fieldName);

        /**
         * Insert a new element before the position 'pos'. Invalidates all previously returned
         * Iterators and Obj. This Obj remain valid.
         *
         * Returns an iterator to the newly inserted element together with the end iterator for this
         * Obj.
         */
        std::pair<Iterator, Iterator> insert(Iterator pos, std::string fieldName);

        /**
         * Iteration for subelements of this Obj.
         */
        Iterator begin();
        Iterator end();
        ConstIterator begin() const;
        ConstIterator end() const;

    private:
        Obj(std::vector<Entry>& entries, std::vector<Entry>::iterator pos);

        Entries& _entries;
        Entries::iterator _pos;
    };

    MinMaxStore();

    /**
     * Access to the root Obj for this store.
     */
    Obj root() {
        return {entries, entries.begin()};
    }

private:
    Entries entries;
};

/**
 * Manages Min and Max values for timeseries measurements within a bucket.
 */
class MinMax {
public:
    /**
     * Updates the min/max fields provided by 'doc', ignoring the 'metaField' field.
     */
    void update(const BSONObj& doc,
                boost::optional<StringData> metaField,
                const StringData::ComparatorInterface* stringComparator);

    /**
     * Returns the full min/max object.
     */
    BSONObj min();
    BSONObj max();

    /**
     * Returns the updates since the previous time this function or the min/max functions were
     * called in the format for an update op.
     */
    BSONObj minUpdates();
    BSONObj maxUpdates();

private:
    // Helper for update() above to provide recursion of MinMax element together with a BSONElement
    std::pair<MinMaxStore::Iterator, MinMaxStore::Iterator> _update(
        MinMaxStore::Obj obj,
        BSONElement elem,
        bool updateMinValues,
        bool updateMaxValues,
        const StringData::ComparatorInterface* stringComparator);

    // Helper to search and compare field names between MinMaxStore::Obj and BSONObj.
    template <typename SkipFieldFn>
    void _updateObj(MinMaxStore::Obj& obj,
                    const BSONObj& doc,
                    bool updateMin,
                    bool updateMax,
                    const StringData::ComparatorInterface* stringComparator,
                    SkipFieldFn skipFieldFn);

    /**
     * Appends the MinMax, to the builder.
     */
    template <typename GetDataFn>
    void _append(MinMaxStore::Obj obj, BSONObjBuilder* builder, GetDataFn getData);
    template <typename GetDataFn>
    void _append(MinMaxStore::Obj obj, BSONArrayBuilder* builder, GetDataFn getData);

    /**
     * Appends updates, if any, to the builder. Returns whether any updates were appended.
     */
    template <typename GetDataFn>
    bool _appendUpdates(MinMaxStore::Obj obj, BSONObjBuilder* builder, GetDataFn getData);

    /**
     * Clears the '_updated' flag on this Iterator and all its subelements.
     */
    template <typename GetDataFn>
    void _clearUpdated(MinMaxStore::Iterator elem, GetDataFn getData);

    template <typename GetDataFn>
    void _setTypeObject(MinMaxStore::Obj& obj, GetDataFn getData);

    template <typename GetDataFn>
    void _setTypeArray(MinMaxStore::Obj& obj, GetDataFn getData);

    /**
     * Helper for the recursive internal functions to access the min data component.
     */
    struct GetMin {
        MinMaxStore::Data& operator()(MinMaxStore::Element& element) const {
            return element.min();
        }
        const MinMaxStore::Data& operator()(const MinMaxStore::Element& element) const {
            return element.min();
        }
    };

    /**
     * Helper for the recursive internal functions to access the max data component.
     */
    struct GetMax {
        MinMaxStore::Data& operator()(MinMaxStore::Element& element) const {
            return element.max();
        }
        const MinMaxStore::Data& operator()(const MinMaxStore::Element& element) const {
            return element.max();
        }
    };

    MinMaxStore _store;
};
}  // namespace mongo::timeseries
