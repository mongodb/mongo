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

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/util/string_map.h"
#include "mongo/util/tracked_types.h"
#include "mongo/util/tracking_context.h"

namespace mongo::timeseries::bucket_catalog {

/**
 * Stores a BSON hierarchy in a flat contigous memory structure. Optimized for fast traversal
 * in lock-step of a BSONObj with the same internal field order. It does this at the expense of
 * insert performance which should be a rare operation when adding measurements to a timeseries
 * bucket. Usually we need to traverse the FlatBSONStore structure to check if we need to update any
 * values, or to check for compatibility with an incoming measurement.
 *
 * Provides search capability when field order is not what was expected and contains a fallback to
 * map lookup when linear search does not find elements within a constant search threshold.
 */
template <class Element, class Value>
class FlatBSONStore {
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
     * Stored data for an Element
     */
    class Data {
    public:
        friend class FlatBSONStore;

        explicit Data(TrackingContext& trackingContext);

        /**
         * DataType stored by this Data
         */
        Type type() const;

        /**
         * Flag to indicate if this FlatBSON::Data was updated since last clear.
         */
        bool updated() const;

        /**
         * Clear update flag.
         */
        void clearUpdated();

        /**
         * Value stored by this Data. Only valid to call if type is kValue.
         */
        const Value& value() const;

        /**
         * Sets the type of this Data.
         */
        void setObject();
        void setArray();
        void setUnset();
        void setValue(const BSONElement& elem);

    private:
        Value _value;
        Type _type = Type::kUnset;
        bool _updated = false;
    };

    class Obj;

    /**
     * Internal storage type to manage iterator offsets needed to implement a tree structure in a
     * flat buffer.
     */
    struct Entry {
    public:
        explicit Entry(TrackingContext& trackingContext);

        // Iterator offset to the entry after the last subelement
        uint32_t _offsetEnd;
        // Iterator offset to the parent entry
        uint32_t _offsetParent;
        // Data bearing element, exposed through public interfaces
        Element _element;
        // Map for faster searches. Contain mapping from field name to iterator offset to
        // subelement. Only instantiated when we've depleted our allowed linear search depth.
        boost::optional<TrackedStringMap<uint32_t>> _fieldNameToIndex;
    };
    using Entries = tracked_vector<Entry>;

    /**
     * Forward iterator over subelements in an Obj.
     */
    class Iterator {
    public:
        friend class FlatBSONStore::Obj;

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
        Iterator(typename Entries::iterator pos);

        typename Entries::iterator _pos;
    };

    /**
     * Forward iterator over subelements in an ObjView.
     */
    class ConstIterator {
    public:
        friend class FlatBSONStore::Obj;

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
        ConstIterator(typename Entries::const_iterator pos);

        typename Entries::const_iterator _pos;
    };


    /**
     * Represents an 'Object' within the FlatBSONStore. Analogous BSONObj for BSON. Provides
     * iteration, insertion and search capability for subelements.
     */
    class Obj {

    public:
        friend class FlatBSONStore;

        /**
         * Copying an Obj copies the internal position. It is not allowed to copy to an Obj owned by
         * a different FlatBSONStore.
         */
        Obj& operator=(const Obj& rhs);

        /**
         * Access to the element containing user-data such as field names and stored values.
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
        Obj(TrackingContext&, Entries& entries, typename Entries::iterator pos);

        Entries& _entries;
        typename Entries::iterator _pos;

        std::reference_wrapper<TrackingContext> _trackingContext;
    };

    explicit FlatBSONStore(TrackingContext&);

    FlatBSONStore(FlatBSONStore&& other) = default;

    FlatBSONStore& operator=(FlatBSONStore&& other) = default;

    /**
     * Access to the root Obj for this store.
     */
    Obj root() {
        return {_trackingContext, entries, entries.begin()};
    }

private:
    Entries entries;

    std::reference_wrapper<TrackingContext> _trackingContext;
};

/**
 * Manages updating and extracting values in a FlatBSONStore.
 */
template <class Derived, class Element, class Value>
class FlatBSON {
public:
    explicit FlatBSON(TrackingContext&);

    FlatBSON(FlatBSON&& other) = default;

    FlatBSON& operator=(FlatBSON&& other) = default;

    enum class UpdateStatus { Updated, Failed, NoChange };
    static std::string updateStatusString(UpdateStatus updateStatus);

    /**
     * Updates the stored fields provided by 'doc', ignoring the 'metaField' field.
     */
    UpdateStatus update(const BSONObj& doc,
                        boost::optional<StringData> metaField,
                        const StringDataComparator* stringComparator);

protected:
    // Helper for update() above to provide recursion of FlatBSONStore element together with a
    // BSONElement
    std::tuple<UpdateStatus,
               typename FlatBSONStore<Element, Value>::Iterator,
               typename FlatBSONStore<Element, Value>::Iterator>
    _update(typename FlatBSONStore<Element, Value>::Obj obj,
            BSONElement elem,
            typename Element::UpdateContext updateContext,
            const StringDataComparator* stringComparator);

    // Helper to search and compare field names between FlatBSONStore::Obj and BSONObj.
    UpdateStatus _updateObj(typename FlatBSONStore<Element, Value>::Obj& obj,
                            const BSONObj& doc,
                            typename Element::UpdateContext updateContext,
                            const StringDataComparator* stringComparator,
                            std::function<bool(StringData)> skipFieldFn);

    /**
     * Appends the BSONObj represented by the FlatBSONStore to the builder.
     */
    template <typename GetDataFn>
    void _append(typename FlatBSONStore<Element, Value>::Obj obj,
                 BSONObjBuilder* builder,
                 GetDataFn getData);
    template <typename GetDataFn>
    void _append(typename FlatBSONStore<Element, Value>::Obj obj,
                 BSONArrayBuilder* builder,
                 GetDataFn getData);

    /**
     * Appends updates, if any, to the builder. Returns whether any updates were appended.
     */
    template <typename GetDataFn>
    bool _appendUpdates(typename FlatBSONStore<Element, Value>::Obj obj,
                        BSONObjBuilder* builder,
                        GetDataFn getData);

    /**
     * Clears the '_updated' flag on this Iterator and all its subelements.
     */
    template <typename GetDataFn>
    void _clearUpdated(typename FlatBSONStore<Element, Value>::Iterator elem, GetDataFn getData);

    template <typename GetDataFn>
    static void _setTypeObject(typename FlatBSONStore<Element, Value>::Obj& obj, GetDataFn getData);

    template <typename GetDataFn>
    static void _setTypeArray(typename FlatBSONStore<Element, Value>::Obj& obj, GetDataFn getData);

    FlatBSONStore<Element, Value> _store;

    std::reference_wrapper<TrackingContext> _trackingContext;
};

/**
 * Buffer value for a Data of type kValue, storing a full BSONElement value.
 */
struct BSONElementValueBuffer {
    explicit BSONElementValueBuffer(TrackingContext&);

    BSONElement get() const;
    void set(const BSONElement&);
    BSONType type() const;
    size_t size() const;

private:
    tracked_vector<char> _buffer;
    size_t _size = 0;
};

/**
 * Base class for an element stored in a FlatBSONStore.
 */
class Element {
public:
    explicit Element(TrackingContext&);

    /**
     * Field name component
     */
    StringData fieldName() const;

    void setFieldName(std::string&& fieldName);

    /**
     * Returns true if this Element is only used for Array storage. Array field names are not
     * used and may be claimed by an Object.
     */
    bool isArrayFieldName() const;
    void claimArrayFieldNameForObject(std::string name);

private:
    tracked_string _fieldName;
};


class MinMaxElement;
typedef FlatBSONStore<MinMaxElement, BSONElementValueBuffer> MinMaxStore;

/**
 * Element representing both the min and max values for a given field path across all measurements
 * in a bucket.
 */
class MinMaxElement : public Element {
public:
    struct UpdateContext {
        bool min = true;
        bool max = true;
    };

    explicit MinMaxElement(TrackingContext&);

    void initializeRoot();

    /**
     * Min data component access
     */
    MinMaxStore::Data& min();
    const MinMaxStore::Data& min() const;

    /**
     * Max data component access
     */
    MinMaxStore::Data& max();
    const MinMaxStore::Data& max() const;

private:
    MinMaxStore::Data _min;
    MinMaxStore::Data _max;
};

/**
 * Manages Min and Max values for timeseries measurements within a bucket.
 */
class MinMax : public FlatBSON<MinMax, MinMaxElement, BSONElementValueBuffer> {
    friend class FlatBSON<MinMax, MinMaxElement, BSONElementValueBuffer>;

public:
    explicit MinMax(TrackingContext&);

    MinMax(MinMax&&) = default;

    MinMax& operator=(MinMax&&) = default;

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

    /**
     * Generates and returns a MinMax object from the passed in min and max documents.
     */
    static MinMax parseFromBSON(TrackingContext&,
                                const BSONObj& min,
                                const BSONObj& max,
                                const StringDataComparator* stringComparator);

protected:
    static std::pair<UpdateStatus, MinMaxElement::UpdateContext> _shouldUpdateObj(
        MinMaxStore::Obj& obj, const BSONElement& elem, MinMaxElement::UpdateContext updateContext);

    static std::pair<UpdateStatus, MinMaxElement::UpdateContext> _shouldUpdateArr(
        MinMaxStore::Obj& obj, const BSONElement& elem, MinMaxElement::UpdateContext updateContext);

    static UpdateStatus _maybeUpdateValue(MinMaxStore::Obj& obj,
                                          const BSONElement& elem,
                                          MinMaxElement::UpdateContext updateContext,
                                          const StringDataComparator* stringComparator);

private:
    /**
     * Helper for the recursive internal functions to access the min data component.
     */
    struct GetMin {
        MinMaxStore::Data& operator()(MinMaxElement& element) const {
            return element.min();
        }
        const MinMaxStore::Data& operator()(const MinMaxElement& element) const {
            return element.min();
        }
    };

    /**
     * Helper for the recursive internal functions to access the max data component.
     */
    struct GetMax {
        MinMaxStore::Data& operator()(MinMaxElement& element) const {
            return element.max();
        }
        const MinMaxStore::Data& operator()(const MinMaxElement& element) const {
            return element.max();
        }
    };
};

/**
 * Buffer value for a Data of type kValue, storing just the BSONElement type.
 */
struct BSONTypeValue {
    BSONTypeValue(TrackingContext&) {}

    BSONElement get() const;
    void set(const BSONElement&);
    BSONType type() const;
    int64_t size() const;

private:
    BSONType _type = BSONType::EOO;
};

class SchemaElement;
typedef FlatBSONStore<SchemaElement, BSONTypeValue> SchemaStore;

/**
 * Element representing schema type for a given field path for all measurements in a bucket.
 */
class SchemaElement : public Element {
public:
    struct UpdateContext {};

    explicit SchemaElement(TrackingContext&);

    void initializeRoot();

    /**
     * Schema data component access
     */
    SchemaStore::Data& data();
    const SchemaStore::Data& data() const;

private:
    SchemaStore::Data _data;
};

/**
 * Manages schema data for timeseries measurements within a bucket.
 */
class Schema : public FlatBSON<Schema, SchemaElement, BSONTypeValue> {
    friend class FlatBSON<Schema, SchemaElement, BSONTypeValue>;

public:
    explicit Schema(TrackingContext&);

    /**
     * Generates and returns a Schema object from the passed in min and max documents.
     */
    static Schema parseFromBSON(TrackingContext&,
                                const BSONObj& min,
                                const BSONObj& max,
                                const StringDataComparator* stringComparator);

protected:
    static std::pair<UpdateStatus, typename SchemaElement::UpdateContext> _shouldUpdateObj(
        SchemaStore::Obj& obj, const BSONElement& elem, SchemaElement::UpdateContext updateContext);

    static std::pair<UpdateStatus, typename SchemaElement::UpdateContext> _shouldUpdateArr(
        SchemaStore::Obj& obj, const BSONElement& elem, SchemaElement::UpdateContext updateContext);

    static UpdateStatus _maybeUpdateValue(SchemaStore::Obj& obj,
                                          const BSONElement& elem,
                                          SchemaElement::UpdateContext updateContext,
                                          const StringDataComparator* stringComparator);

private:
    /**
     * Helper for the recursive internal functions to access the type data component.
     */
    struct GetData {
        SchemaStore::Data& operator()(SchemaElement& element) const {
            return element.data();
        }
        const SchemaStore::Data& operator()(const SchemaElement& element) const {
            return element.data();
        }
    };
};

}  // namespace mongo::timeseries::bucket_catalog
