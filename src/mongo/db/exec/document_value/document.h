/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include "mongo/base/string_data_comparator.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/document_value/document_internal.h"
#include "mongo/db/exec/document_value/document_metadata_fields.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_internal.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/string_map.h"

#include <cstring>
#include <initializer_list>
#include <iosfwd>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

#include <boost/functional/hash.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
class BSONObj;

class FieldIterator;
class FieldPath;
class Value;
class MutableDocument;
/** An internal class that represents the position of a field in a document.
 *
 *  This is a low-level class that you usually don't need to worry about.
 *
 *  The main use of this class for clients is to allow refetching or
 *  setting a field without looking it up again. It has a default
 *  constructor that represents a field not being in a document. It also
 *  has a method 'bool found()' that tells you if a field was found.
 *
 *  For more details see document_internal.h
 */
class Position;

/** A Document is similar to a BSONObj but with a different in-memory representation.
 *
 *  A Document can be treated as a const std::map<std::string, const Value> that is
 *  very cheap to copy and is Assignable.  Therefore, it is acceptable to
 *  pass and return by Value. Note that the data in a Document is
 *  immutable, but you can replace a Document instance with assignment.
 *
 *  See Also: Value class in Value.h
 */
class Document {
public:
    /**
     * Operator overloads for relops return a DeferredComparison which can subsequently be evaluated
     * by a DocumentComparator.
     */
    struct DeferredComparison {
        enum class Type {
            kLT,
            kLTE,
            kEQ,
            kGT,
            kGTE,
            kNE,
        };

        DeferredComparison(Type type, const Document& lhs, const Document& rhs)
            : type(type), lhs(lhs), rhs(rhs) {}

        Type type;
        const Document& lhs;
        const Document& rhs;
    };

    static constexpr StringData metaFieldTextScore = "$textScore"_sd;
    static constexpr StringData metaFieldRandVal = "$randVal"_sd;
    static constexpr StringData metaFieldSortKey = "$sortKey"_sd;
    static constexpr StringData metaFieldGeoNearDistance = "$dis"_sd;
    static constexpr StringData metaFieldGeoNearPoint = "$pt"_sd;
    static constexpr StringData metaFieldSearchScore = "$searchScore"_sd;
    static constexpr StringData metaFieldSearchHighlights = "$searchHighlights"_sd;
    static constexpr StringData metaFieldSearchScoreDetails = "$searchScoreDetails"_sd;
    static constexpr StringData metaFieldSearchRootDocumentId = "$searchRootDocumentId"_sd;
    static constexpr StringData metaFieldSearchSortValues = "$searchSortValues"_sd;
    static constexpr StringData metaFieldIndexKey = "$indexKey"_sd;
    static constexpr StringData metaFieldVectorSearchScore = "$vectorSearchScore"_sd;
    static constexpr StringData metaFieldSearchSequenceToken = "$searchSequenceToken"_sd;
    static constexpr StringData metaFieldScore = "$score"_sd;
    static constexpr StringData metaFieldScoreDetails = "$scoreDetails"_sd;
    static constexpr StringData metaFieldStream = "$stream"_sd;
    static constexpr StringData metaFieldChangeStreamControlEvent = "$changeStreamControlEvent"_sd;

    static const StringDataSet allMetadataFieldNames;

    /// Empty Document (does no allocation)
    Document() {}

    /// Defaulted copy and move constructors.
    Document(const Document&) = default;
    Document(Document&&) = default;

    /// Create a new Document deep-converted from the given BSONObj.
    explicit Document(const BSONObj& bson);

    /**
     * Create a new document from key, value pairs. Enables constructing a document using this
     * syntax:
     * auto document = Document{{"hello", "world"}, {"number", 1}};
     */
    Document(std::initializer_list<std::pair<StringData, ImplicitValue>> initializerList);
    Document(const std::vector<std::pair<StringData, Value>>& fields);

    void swap(Document& rhs) {
        _storage.swap(rhs._storage);
    }

    /// Defaulted copy and move assignment operators.
    Document& operator=(const Document&) = default;
    Document& operator=(Document&&) = default;

    /**
     * Look up a field by key name. Returns Value() if no such field. O(1).
     *
     * Note that this method does *not* traverse nested documents and arrays, use getNestedField()
     * instead.
     */
    template <AnyFieldNameTypeButStdString T>
    Value operator[](T key) const {
        return getField(key);
    }
    template <AnyFieldNameTypeButStdString T>
    Value getField(T key) const {
        return storage().getField(key);
    }

    /// Look up a field by Position. See positionOf and getNestedField.
    Value getField(Position pos) const {
        return storage().getField(pos).val;
    }

    /**
     * Returns the Value stored at the location given by 'path', or Value() if no such path exists.
     * If 'positions' is non-null, it will be filled with a path suitable to pass to
     * MutableDocument::setNestedField().
     */
    Value getNestedField(const FieldPath& path, std::vector<Position>* positions = nullptr) const;

    /**
     * Returns field at given path coerced to a Value. If an array is encountered in the middle of
     * the path or at the leaf then boost::none is returned.
     *
     * If the field is not found, an empty Value() is returned.
     */
    boost::optional<Value> getNestedScalarFieldNonCaching(const FieldPath& dottedField) const;

    // Number of fields in this document. Exp. runtime O(n).
    size_t computeSize() const {
        return storage().computeSize();
    }

    /// True if this document has no fields.
    bool empty() const {
        return !_storage || storage().iterator().atEnd();
    }

    /// Create a new FieldIterator that can be used to examine the Document's fields in order.
    FieldIterator fieldIterator() const;

    /// Convenience type for dealing with fields. Used by FieldIterator.
    typedef std::pair<StringData, Value> FieldPair;

    /**
     * Get the approximate size of the Document, plus its underlying storage and sub-values. Returns
     * size in bytes. The return value of this function is snapshotted. All subsequent calls of this
     * method will return the same value.
     *
     * Note: Some memory may be shared with other Documents or between fields within a single
     * Document so this can overestimate usage.
     *
     * Note: the value returned by this function includes the size of the metadata associated with
     * the document.
     */
    size_t getApproximateSize() const;

    /**
     * Same as 'getApproximateSize()', but this method re-computes the size on every call.
     */
    size_t getCurrentApproximateSize() const;

    /**
     * Return the approximate amount of space used by metadata.
     */
    size_t getMetadataApproximateSize() const {
        return storage().getMetadataApproximateSize();
    }

    /**
     * Compare two documents. Most callers should prefer using DocumentComparator instead. See
     * document_comparator.h for details.
     *
     *  BSON document field order is significant, so this just goes through
     *  the fields in order.  The comparison is done in roughly the same way
     *  as strings are compared, but comparing one field at a time instead
     *  of one character at a time.
     *
     *  Pass a non-null StringDataComparator if special string comparison semantics are
     *  required. If the comparator is null, then a simple binary compare is used for strings. This
     *  comparator is only used for string *values*; field names are always compared using simple
     *  binary compare.
     *
     *  Note: This does not consider metadata when comparing documents.
     *
     *  @returns an integer less than zero, zero, or an integer greater than
     *           zero, depending on whether lhs < rhs, lhs == rhs, or lhs > rhs
     *  Warning: may return values other than -1, 0, or 1
     */
    static int compare(const Document& lhs,
                       const Document& rhs,
                       const StringDataComparator* stringComparator);

    /**
     * Merge two documents.
     * Sub-documents with the same key are merged together, otherwise the value in 'rhs' overwrites
     * the one in 'lhs'.
     *
     * Note: Arrays are treated as non-object types, but that can be changed if needed.
     *
     * @returns a new document resulting after the merge.
     */
    static Document deepMerge(const Document& lhs, const Document& rhs);

    std::string toString() const;

    friend std::ostream& operator<<(std::ostream& out, const Document& doc) {
        return out << doc.toString();
    }

    /**
     * Returns a cache-only copy of the document with no backing bson.
     */
    Document shred() const {
        return storage().shred();
    }

    /**
     * Loads the whole document into cache.
     */
    void loadIntoCache() const {
        return storage().loadIntoCache();
    }

    /** Calculate a hash value.
     *
     * Meant to be used to create composite hashes suitable for
     * hashed container classes such as unordered_map.
     */
    void hash_combine(size_t& seed, const StringDataComparator* stringComparator) const;

    /**
     * Returns true, if this document is trivially convertible to BSON, meaning the underlying
     * storage is already in BSON format and there are no damages.
     */
    bool isTriviallyConvertible() const {
        return !storage().isModified() && !storage().bsonHasMetadata();
    }

    /**
     * Returns true, if this document is trivially convertible to BSON with metadata, meaning the
     * underlying storage is already in BSON format and there are no damages.
     */
    bool isTriviallyConvertibleWithMetadata() const {
        return !storage().isModified() && !storage().isMetadataModified();
    }

    /**
     * Validates that the size of the document as a BSON object does not exceed maxSize limit, and
     * throws BSONObjectTooLarge otherwise.
     */
    static void validateDocumentBSONSize(const BSONObj& docBSONObj, int maxSize) {
        uassertStatusOK(
            docBSONObj.validateBSONObjSize(maxSize).addContext("Serializing Document failed"));
    }

    /**
     * Serializes this document to the BSONObj under construction in 'builder'. Metadata is not
     * included. Throws a AssertionException if 'recursionLevel' exceeds the maximum allowable
     * depth.
     */
    void toBson(BSONObjBuilder* builder, size_t recursionLevel = 1) const;

    template <typename BSONTraits = BSONObj::DefaultSizeTrait>
    BSONObj toBson() const {
        if (isTriviallyConvertible()) {
            return storage().bsonObj();
        }

        BSONObjBuilder bb;
        toBson(&bb);
        BSONObj docBSONObj = bb.obj<BSONTraits>();
        validateDocumentBSONSize(docBSONObj, BSONTraits::MaxSize);
        return docBSONObj;
    }

    /**
     * Serializes this document iff the conversion is "trivial," meaning that the underlying storage
     * is already in BSON format and there are no damages. No conversion is necessary; this function
     * just returns the already existing BSON.
     *
     * When the trivial conversion is not possible, this function returns boost::none.
     */
    boost::optional<BSONObj> toBsonIfTriviallyConvertible() const;

    /**
     * Like the 'toBson()' method, but includes metadata as top-level fields.
     */
    void toBsonWithMetaData(BSONObjBuilder* builder) const;

    template <typename BSONTraits = BSONObj::DefaultSizeTrait>
    BSONObj toBsonWithMetaData() const {
        if (isTriviallyConvertibleWithMetadata()) {
            return storage().bsonObj();
        }

        BSONObjBuilder bb;
        toBsonWithMetaData(&bb);
        BSONObj docBSONObj = bb.obj<BSONTraits>();
        validateDocumentBSONSize(docBSONObj, BSONTraits::MaxSize);
        return docBSONObj;
    }

    /**
     * Like Document(BSONObj) but treats top-level fields with special names as metadata.
     * Special field names are available as static constants on this class with names starting
     * with metaField.
     */
    static Document fromBsonWithMetaData(const BSONObj& bson);

    // Support BSONObjBuilder and BSONArrayBuilder "stream" API
    friend void appendToBson(BSONObjBuilder& builder, StringData fieldName, const Document& doc) {
        BSONObjBuilder subobj(builder.subobjStart(fieldName));
        doc.toBson(&subobj);
        subobj.doneFast();
    }

    /** Return the abstract Position of a field, suitable to pass to operator[] or getField().
     *  This can potentially save time if you need to refer to a field multiple times.
     */
    Position positionOf(StringData fieldName) const {
        return storage().findField(fieldName);
    }

    /** Clone a document.
     *
     *  This should only be called by MutableDocument and tests
     *
     *  The new document shares all the fields' values with the original.
     *  This is not a deep copy.  Only the fields on the top-level document
     *  are cloned.
     */
    Document clone() const {
        return Document(storage().clone().get());
    }

    /**
     * Returns a const reference to an object housing the metadata fields associated with this
     * WorkingSetMember.
     */
    const DocumentMetadataFields& metadata() const {
        return storage().metadata();
    }

    /// members for Sorter
    struct SorterDeserializeSettings {};  // unused
    void serializeForSorter(BufBuilder& buf) const;
    static Document deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&);

    /**
     * Returns the amount of memory used by this 'Document' instance when serialized, e.g. when
     * serialized as BSON for returning to a client or when serialized for spilling to disk. This
     * can differ substantially from 'getApproximateSize()' due to the fact that not all portions of
     * the backing BSON may appear in the serialized version of the document.
     */
    size_t memUsageForSorter() const;

    /**
     * Returns a document that owns the underlying BSONObj.
     */
    Document getOwned() const&;
    Document getOwned() &&;

    /**
     * Needed to satisfy the Sorter interface. This method throws an assertion.
     */
    void makeOwned() {
        MONGO_UNREACHABLE;
    }

    /**
     * Returns true if the underlying BSONObj is owned.
     */
    bool isOwned() const {
        return _storage ? _storage->isOwned() : true;
    }

    /**
     * Returns true if the document has been modified (i.e. it differs from the underlying BSONObj).
     */
    auto isModified() const {
        return _storage ? _storage->isModified() : false;
    }

    bool hasExclusivelyOwnedStorage() const {
        return _storage && !_storage->isShared();
    }

    /// only for testing
    const void* getPtr() const {
        return _storage.get();
    }

private:
    friend class FieldIterator;
    friend class ValueStorage;
    friend class MutableDocument;
    friend class MutableValue;

    explicit Document(boost::intrusive_ptr<const DocumentStorage>&& ptr)
        : _storage(std::move(ptr)) {}

    const DocumentStorage& storage() const {
        return (_storage ? *_storage : DocumentStorage::emptyDoc());
    }

    boost::optional<Value> getNestedScalarFieldNonCachingHelper(const FieldPath& dottedField,
                                                                size_t level) const;

    boost::intrusive_ptr<const DocumentStorage> _storage;
};

//
// Comparison API.
//
// Document instances can be compared either using Document::compare() or via operator overloads.
// Most callers should prefer operator overloads. Note that the operator overloads return a
// DeferredComparison, which must be subsequently evaluated by a DocumentComparator. See
// document_comparator.h for details.
//

inline Document::DeferredComparison operator==(const Document& lhs, const Document& rhs) {
    return Document::DeferredComparison(Document::DeferredComparison::Type::kEQ, lhs, rhs);
}

inline Document::DeferredComparison operator!=(const Document& lhs, const Document& rhs) {
    return Document::DeferredComparison(Document::DeferredComparison::Type::kNE, lhs, rhs);
}

inline Document::DeferredComparison operator<(const Document& lhs, const Document& rhs) {
    return Document::DeferredComparison(Document::DeferredComparison::Type::kLT, lhs, rhs);
}

inline Document::DeferredComparison operator<=(const Document& lhs, const Document& rhs) {
    return Document::DeferredComparison(Document::DeferredComparison::Type::kLTE, lhs, rhs);
}

inline Document::DeferredComparison operator>(const Document& lhs, const Document& rhs) {
    return Document::DeferredComparison(Document::DeferredComparison::Type::kGT, lhs, rhs);
}

inline Document::DeferredComparison operator>=(const Document& lhs, const Document& rhs) {
    return Document::DeferredComparison(Document::DeferredComparison::Type::kGTE, lhs, rhs);
}

/** This class is returned by MutableDocument to allow you to modify its values.
 *  You are not allowed to hold variables of this type (enforced by the type system).
 */
class MutableValue {
public:
    void operator=(const Value& v) {
        _val = v;
    }

    void operator=(Value&& v) {
        _val = std::move(v);
    }

    /** These are designed to allow things like mutDoc["a"]["b"]["c"] = Value(10);
     *  It is safe to use even on nonexistent fields.
     */
    MutableValue operator[](StringData key) {
        return getField(key);
    }
    MutableValue operator[](Position pos) {
        return getField(pos);
    }

    MutableValue getField(StringData key);
    MutableValue getField(Position pos);

private:
    friend class MutableDocument;

    /// can only be constructed or copied by self and friends
    MutableValue(const MutableValue& other) : _val(other._val) {}
    explicit MutableValue(Value& val) : _val(val) {}

    /// Used by MutableDocument(MutableValue)
    const RefCountable*& getDocPtr() {
        if (_val.getType() != BSONType::object || _val._storage.genericRCPtr == nullptr) {
            // If the current value isn't an object we replace it with a Object-typed Value.
            // Note that we can't just use Document() here because that is a NULL pointer and
            // Value doesn't refcount NULL pointers. This led to a memory leak (SERVER-10554)
            // because MutableDocument::newStorage() would set a non-NULL pointer into the Value
            // without setting the refCounter bit. While allocating a DocumentStorage here could
            // result in an allocation where none is needed, in practice this is only called
            // when we are about to add a field to the sub-document so this just changes where
            // the allocation is done.
            _val = Value(Document(make_intrusive<DocumentStorage>()));
        }

        return _val._storage.genericRCPtr;
    }

    MutableValue& operator=(const MutableValue&);  // not assignable with another MutableValue

    Value& _val;
};

/** MutableDocument is a Document builder that supports both adding and updating fields.
 *
 *  This class fills a similar role to BSONObjBuilder, but allows you to
 *  change existing fields and more easily write to sub-Documents.
 *
 *  To preserve the immutability of Documents, MutableDocument will
 *  shallow-clone its storage on write (COW) if it is shared with any other
 *  Documents.
 */
class MutableDocument {
    MutableDocument(const MutableDocument&) = delete;
    MutableDocument& operator=(const MutableDocument&) = delete;

public:
    /** Create a new empty Document.
     *
     *  @param expectedFields a hint at what the number of fields will be, if known.
     *         this can be used to increase memory allocation efficiency. There is
     *         no impact on correctness if this field over or under estimates.
     *
     *  TODO: find some way to convey field-name sizes to make even more efficient
     */
    MutableDocument() : _storageHolder(nullptr), _storage(_storageHolder) {}
    explicit MutableDocument(size_t expectedFields);

    /// No copy of data yet. Copy-on-write. See storage()
    explicit MutableDocument(Document d) : _storageHolder(nullptr), _storage(_storageHolder) {
        reset(std::move(d));
    }

    ~MutableDocument() {
        if (_storageHolder)
            intrusive_ptr_release(_storageHolder);
    }

    /** Replace the current base Document with the argument
     *
     *  All Positions from the passed in Document are valid and refer to the
     *  same field in this MutableDocument.
     */
    void reset(Document d = Document()) {
        reset(std::move(d._storage));
    }

    /**
     * Replace the current base Document with the BSON object. Setting 'bsonHasMetadata' to true
     * signals that the BSON object contains metadata fields.
     */
    void reset(const BSONObj& bson, bool bsonHasMetadata) {
        storage().reset(bson, bsonHasMetadata);
    }

    /** Add the given field to the Document.
     *
     *  BSON documents' fields are ordered; the new Field will be
     *  appended to the current list of fields.
     *
     *  Unlike getField/setField, addField does not look for a field with the
     *  same name and therefore cannot be used to update fields.
     *
     *  It is an error to add a field that has the same name as another field.
     *
     *  TODO: This is currently allowed but getField only gets first field.
     *        Decide what level of support is needed for duplicate fields.
     *        If duplicates are not allowed, consider removing this method.
     */
    void addField(StringData name, const Value& val) {
        storage().appendField(name, ValueElement::Kind::kInserted) = val;
    }
    void addField(HashedFieldName field, const Value& val) {
        storage().appendField(field, ValueElement::Kind::kInserted) = val;
    }

    void addField(StringData name, Value&& val) {
        storage().appendField(name, ValueElement::Kind::kInserted) = std::move(val);
    }
    void addField(HashedFieldName field, Value&& val) {
        storage().appendField(field, ValueElement::Kind::kInserted) = std::move(val);
    }

    /** Update field by key. If there is no field with that key, add one.
     *
     *  If the new value is missing(), the field is logically removed.
     */
    MutableValue operator[](StringData key) {
        return getField(key);
    }
    void setField(StringData key, const Value& val) {
        getField(key) = val;
    }
    void setField(StringData key, Value&& val) {
        getField(key) = std::move(val);
    }
    MutableValue getField(StringData key) {
        return MutableValue(storage().getFieldCacheOnlyOrCreate(key));
    }
    MutableValue getFieldNonLeaf(StringData key) {
        return MutableValue(storage().getFieldOrCreate(key));
    }

    /// Update field by Position. Must already be a valid Position.
    MutableValue operator[](Position pos) {
        return getField(pos);
    }
    void setField(Position pos, const Value& val) {
        getField(pos) = val;
    }
    void setField(Position pos, Value&& val) {
        getField(pos) = std::move(val);
    }
    MutableValue getField(Position pos) {
        return MutableValue(storage().getField(pos).val);
    }

    /// Logically remove a field. Note that memory usage does not decrease.
    void remove(StringData key) {
        getField(key) = Value();
    }
    void removeNestedField(const std::vector<Position>& positions) {
        getNestedField(positions) = Value();
    }

    /** Gets/Sets a nested field given a path.
     *
     *  All fields along path are created as empty Documents if they don't exist or are any other
     * type. Does *not* traverse nested arrays when evaluating a nested path, instead returning
     * Value() if the dotted field points to a nested object within an array.
     */
    MutableValue getNestedField(const FieldPath& dottedField);
    void setNestedField(const FieldPath& dottedField, const Value& val) {
        getNestedField(dottedField) = val;
    }
    void setNestedField(const FieldPath& dottedField, Value&& val) {
        getNestedField(dottedField) = std::move(val);
    }

    /// Takes positions vector from Document::getNestedField. All fields in path must exist.
    MutableValue getNestedField(const std::vector<Position>& positions);
    void setNestedField(const std::vector<Position>& positions, const Value& val) {
        getNestedField(positions) = val;
    }
    void setNestedField(const std::vector<Position>& positions, Value&& val) {
        getNestedField(positions) = std::move(val);
    }

    /**
     * Copies all metadata from source if it has any.
     * Note: does not clear metadata from this.
     */
    void copyMetaDataFrom(const Document& source) {
        storage().copyMetaDataFrom(source.storage());
    }

    /**
     * Returns a non-const reference to an object housing the metadata fields associated with this
     * WorkingSetMember.
     */
    DocumentMetadataFields& metadata() {
        return storage().metadata();
    }

    /**
     * Clears all metadata fields inside this Document, and returns a structure containing that
     * extracted metadata to the caller. The metadata can then be attached to a new Document or to
     * another data structure that houses metadata.
     */
    DocumentMetadataFields releaseMetadata() {
        return storage().releaseMetadata();
    }

    /**
     * Transfers metadata fields to this Document. By pairs of calls to releaseMetadata() and
     * setMetadata(), callers can cheaply transfer metadata between Documents.
     */
    void setMetadata(DocumentMetadataFields&& metadata) {
        storage().setMetadata(std::move(metadata));
    }

    /** Convert to a read-only document and release reference.
     *
     *  Call this to indicate that you are done with this Document and will
     *  not be making further changes from this MutableDocument.
     *
     *  TODO: there are some optimizations that may make sense at freeze time.
     */
    Document freeze() {
        resetSnapshottedApproximateSize();
        // This essentially moves _storage into a new Document by way of temp.
        Document ret;
        boost::intrusive_ptr<const DocumentStorage> temp(storagePtr(), /*inc_ref_count=*/false);
        temp.swap(ret._storage);
        _storage = nullptr;
        return ret;
    }

    /// Used to simplify the common pattern of creating a value of the document.
    Value freezeToValue() {
        return Value(freeze());
    }

    /** Borrow a readable reference to this Document.
     *
     *  Note that unlike freeze(), this indicates intention to continue
     *  modifying this document. The returned Document will not observe
     *  future changes to this MutableDocument.
     *
     *  Note that the computed snapshotted approximate size of the Document
     *  is not preserved across calls.
     */
    Document peek() {
        resetSnapshottedApproximateSize();
        return Document(storagePtr());
    }

    size_t getApproximateSize() {
        return peek().getApproximateSize();
    }

    /**
     * Returns true if the underlying BSONObj is owned.
     */
    bool isOwned() {
        return storage().isOwned();
    }

    /**
     * Takes the ownership of the underlying BSONObj if it is not owned.
     */
    void makeOwned() {
        storage().makeOwned();
    }

private:
    friend class MutableValue;  // for access to next constructor
    explicit MutableDocument(MutableValue mv) : _storageHolder(nullptr), _storage(mv.getDocPtr()) {}

    void reset(boost::intrusive_ptr<const DocumentStorage> ds) {
        if (_storage)
            intrusive_ptr_release(_storage);
        _storage = ds.detach();
    }

    // This is split into 3 functions to speed up the fast-path
    DocumentStorage& storage() {
        if (MONGO_unlikely(!_storage))
            return newStorage();

        if (MONGO_unlikely(_storage->isShared()))
            return clonedStorage();

        // This function exists to ensure this is safe
        return const_cast<DocumentStorage&>(*storagePtr());
    }
    DocumentStorage& newStorage() {
        reset(make_intrusive<DocumentStorage>());
        return const_cast<DocumentStorage&>(*storagePtr());
    }
    DocumentStorage& clonedStorage() {
        reset(storagePtr()->clone());
        return const_cast<DocumentStorage&>(*storagePtr());
    }

    // recursive helpers for same-named public methods
    MutableValue getNestedFieldHelper(const FieldPath& dottedField, size_t level);
    MutableValue getNestedFieldHelper(const std::vector<Position>& positions, size_t level);

    // this should only be called by storage methods and peek/freeze/resetsnapshottedApproximateSize
    const DocumentStorage* storagePtr() const {
        dassert(!_storage || typeid(*_storage) == typeid(const DocumentStorage));
        return static_cast<const DocumentStorage*>(_storage);
    }

    void resetSnapshottedApproximateSize() {
        auto mutableStorage = const_cast<DocumentStorage*>(storagePtr());
        if (mutableStorage) {
            mutableStorage->resetSnapshottedApproximateSize();
        }
    }

    // These are both const to prevent modifications bypassing storage() method.
    // They always point to NULL or an object with dynamic type DocumentStorage.
    const RefCountable* _storageHolder;  // Only used in constructors and destructor
    const RefCountable*& _storage;  // references either above member or genericRCPtr in a Value
};

/// This is the public iterator over a document
class FieldIterator {
public:
    explicit FieldIterator(const Document& doc) : _doc(doc), _it(_doc.storage().iterator()) {}

    /// Ask if there are more fields to return.
    bool more() const {
        return !_it.atEnd();
    }

    /// Get next item and advance iterator
    Document::FieldPair next() {
        MONGO_verify(more());

        Document::FieldPair fp(_it->nameSD(), _it->val);
        _it.advance();
        return fp;
    }

    /**
     * Returns the name of the field the iterator currently points to, without advancing.
     */
    StringData fieldName() {
        invariant(more());
        return _it.fieldName();
    }

    /**
     * Advance the iterator without accessing the current Value.
     */
    void advance() {
        _it.advance();
    }

private:
    // We'll hang on to the original document to ensure we keep its storage alive
    Document _doc;
    DocumentStorageIterator _it;
};

/// Macro to create Document literals. Syntax is the same as the BSON("name" << 123) macro.
#define DOC(fields) ((DocumentStream() << fields).done())

/** Macro to create Array-typed Value literals.
 *  Syntax is the same as the BSON_ARRAY(123 << "foo") macro.
 */
#define DOC_ARRAY(fields) ((ValueArrayStream() << fields).done())


// These classes are only for the implementation of the DOC and DOC_ARRAY macros.
// They should not be used for any other reason.
class DocumentStream {
    // The stream alternates between DocumentStream taking a fieldname
    // and ValueStream taking a Value.
    class ValueStream {
    public:
        DocumentStream& operator<<(const Value& val) {
            builder._md[name] = val;
            return builder;
        }

        /// support anything directly supported by a value constructor
        template <typename T>
        DocumentStream& operator<<(const T& val) {
            return *this << Value(val);
        }

        DocumentStream& builder;
        StringData name;
    };

public:
    ValueStream operator<<(StringData name) {
        return ValueStream{*this, name};
    }

    Document done() {
        return _md.freeze();
    }

private:
    MutableDocument _md;
};

class ValueArrayStream {
public:
    ValueArrayStream& operator<<(const Value& val) {
        _array.push_back(val);
        return *this;
    }

    /// support anything directly supported by a value constructor
    template <typename T>
    ValueArrayStream& operator<<(const T& val) {
        return *this << Value(val);
    }

    Value done() {
        return Value(std::move(_array));
    }

private:
    std::vector<Value> _array;
};

inline void swap(mongo::Document& lhs, mongo::Document& rhs) {
    lhs.swap(rhs);
}

/* ======================= INLINED IMPLEMENTATIONS ========================== */

inline FieldIterator Document::fieldIterator() const {
    return FieldIterator(*this);
}

inline MutableValue MutableValue::getField(Position pos) {
    return MutableDocument(*this).getField(pos);
}
inline MutableValue MutableValue::getField(StringData key) {
    return MutableDocument(*this).getField(key);
}
}  // namespace mongo
