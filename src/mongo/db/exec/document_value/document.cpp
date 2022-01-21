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

#include "mongo/db/exec/document_value/document.h"

#include <boost/functional/hash.hpp>

#include "mongo/bson/bson_depth.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/util/str.h"

namespace mongo {
using boost::intrusive_ptr;
using std::string;
using std::vector;

namespace {
/**
 * Assert that a given field path does not exceed the length limit.
 */
void assertFieldPathLengthOK(const FieldPath& path) {
    uassert(5984700,
            "Field path exceeds path length limit",
            path.getPathLength() < BSONDepth::getMaxAllowableDepth());
}
void assertFieldPathLengthOK(const std::vector<Position>& path) {
    uassert(5984701,
            "Field path exceeds path length limit",
            path.size() < BSONDepth::getMaxAllowableDepth());
}

stdx::variant<BSONElement, Value, Document::TraversesArrayTag, stdx::monostate>
getNestedFieldHelperBSON(BSONElement elt, const FieldPath& fp, size_t level) {
    if (level == fp.getPathLength()) {
        if (elt.ok()) {
            return elt;
        } else {
            return stdx::monostate{};
        }
    }

    if (elt.type() == BSONType::Array) {
        return Document::TraversesArrayTag{};
    } else if (elt.type() == BSONType::Object) {
        auto subFieldElt = elt.embeddedObject()[fp.getFieldName(level)];
        return getNestedFieldHelperBSON(subFieldElt, fp, level + 1);
    }

    // The path continues "past" a scalar, and therefore does not exist.
    return stdx::monostate{};
}
}  // namespace

const DocumentStorage DocumentStorage::kEmptyDoc;

const StringDataSet Document::allMetadataFieldNames{Document::metaFieldTextScore,
                                                    Document::metaFieldRandVal,
                                                    Document::metaFieldSortKey,
                                                    Document::metaFieldGeoNearDistance,
                                                    Document::metaFieldGeoNearPoint,
                                                    Document::metaFieldSearchScore,
                                                    Document::metaFieldSearchHighlights,
                                                    Document::metaFieldIndexKey,
                                                    Document::metaFieldSearchScoreDetails};

DocumentStorageIterator::DocumentStorageIterator(DocumentStorage* storage, BSONObjIterator bsonIt)
    : _bsonIt(std::move(bsonIt)),
      _first(storage->begin()),
      _it(_first),
      _end(storage->end()),
      _storage(storage) {
    while (shouldSkipDeleted()) {
        advanceOne();
    }
}

const ValueElement& DocumentStorageIterator::constructInCache() {
    // First construct the ValueElement in the document storage.
    auto pos = _storage->constructInCache(*_bsonIt);

    // The storage may have reallocated memory so update out pointers.
    _first = _storage->begin();
    _end = _storage->end();
    _it = _first->plusBytes(pos.index);

    return *_it;
}

void DocumentStorageIterator::advance() {
    if (atEnd()) {
        return;
    }

    do {
        advanceOne();
    } while (shouldSkipDeleted());
}

bool DocumentStorageIterator::shouldSkipDeleted() {
    if (_bsonIt.more()) {
        const auto fieldName = (*_bsonIt).fieldNameStringData();

        // If we strip the metadata see if a field name matches the known list. All metadata fields
        // start with '$' so optimize for a quick bailout.
        if (_storage->stripMetadata() && fieldName[0] == '$' &&
            Document::allMetadataFieldNames.contains(fieldName)) {
            return true;
        }
        // Check if the field is in the cache and if so then check if it has been deleted (i.e. the
        // val.missing() is true).
        if (auto pos = _storage->findFieldInCache(fieldName); pos.found()) {
            _it = _first->plusBytes(pos.index);
            if (_it->kind == ValueElement::Kind::kMaybeInserted) {
                // We have found the value in the BSON so it was not in fact inserted.
                const_cast<ValueElement*>(_it)->kind = ValueElement::Kind::kCached;
            }
            if (_it->val.missing()) {
                return true;
            }
        } else {
            // This is subtle. The field is coming from the underlying BSON but it is not in the
            // cache. We set the _it pointer to nullptr here so if anybody asks for its value (by
            // dereferencing the iterator) the get() method will call constructInCache().
            // We don't want to construct the object here as in many cases caller simply loop over
            // the iterator without dereferencing.
            _it = nullptr;
        }
    } else if (!atEnd()) {
        if (_it->val.missing() || _it->kind == ValueElement::Kind::kCached) {
            return true;
        }
    }

    return false;
}

template <typename T>
Position DocumentStorage::findFieldInCache(T requested) const {
    int reqSize = requested.size();  // get size calculation out of the way if needed

    if (_numFields >= HASH_TAB_MIN) {  // hash lookup
        const unsigned hash = hashKey(requested);
        const unsigned bucket = hash & _hashTabMask;

        Position pos = _hashTab[bucket];
        while (pos.found()) {
            const ValueElement& elem = getField(pos);
            if (elem.nameLen == reqSize && memcmp(requested.rawData(), elem._name, reqSize) == 0) {
                return pos;
            }

            // possible collision
            pos = elem.nextCollision;
        }
    } else {  // linear scan
        for (auto it = iteratorCacheOnly(); !it.atEnd(); it.advance()) {
            if (it->nameLen == reqSize && memcmp(requested.rawData(), it->_name, reqSize) == 0) {
                return it.position();
            }
        }
    }

    // if we got here, there's no such field
    return Position();
}
template Position DocumentStorage::findFieldInCache<StringData>(StringData field) const;
template Position DocumentStorage::findFieldInCache<HashedFieldName>(HashedFieldName field) const;

template <typename T>
Position DocumentStorage::findField(T field, LookupPolicy policy) const {
    if (auto pos = findFieldInCache(field); pos.found() || policy == LookupPolicy::kCacheOnly) {
        return pos;
    }

    for (auto&& bsonElement : _bson) {
        if (field == bsonElement.fieldNameStringData()) {
            return const_cast<DocumentStorage*>(this)->constructInCache(bsonElement);
        }
    }

    // if we got here, there's no such field
    return Position();
}
template Position DocumentStorage::findField<StringData>(StringData field,
                                                         LookupPolicy policy) const;
template Position DocumentStorage::findField<HashedFieldName>(HashedFieldName field,
                                                              LookupPolicy policy) const;

Position DocumentStorage::constructInCache(const BSONElement& elem) {
    auto savedModified = _modified;
    auto pos = getNextPosition();
    const auto fieldName = elem.fieldNameStringData();
    // This is the only place in the code that we bring a field from the backing BSON into the
    // cache. From this point out, we will not use the backing BSON for this element. Hence,
    // we account for using these many bytes of the backing BSON to be handled in the cache.
    _numBytesFromBSONInCache += elem.size();
    appendField(fieldName, ValueElement::Kind::kCached) = Value(elem);
    _modified = savedModified;

    return pos;
}

template <typename T>
Value& DocumentStorage::appendField(T field, ValueElement::Kind kind) {
    Position pos = getNextPosition();
    const int nameSize = field.size();

    // these are the same for everyone
    const Position nextCollision;
    const Value value;

    // Make room for new field (and padding at end for alignment)
    const unsigned newUsed = ValueElement::align(_usedBytes + sizeof(ValueElement) + nameSize);
    if (_cache + newUsed > _cacheEnd)
        alloc(newUsed);
    _usedBytes = newUsed;

    // Append structure of a ValueElement
    char* dest = _cache + pos.index;  // must be after alloc since it changes _cache
#define append(x)                  \
    memcpy(dest, &(x), sizeof(x)); \
    dest += sizeof(x)
    append(value);
    append(nextCollision);
    append(nameSize);
    append(kind);
    field.copyTo(dest, true);
// Padding for alignment handled above
#undef append

    // Make sure next field starts where we expect it
    fassert(16486, getField(pos).next()->ptr() == _cache + _usedBytes);

    _numFields++;

    if (_numFields > HASH_TAB_MIN) {
        addFieldToHashTable(field, pos);
    } else if (_numFields == HASH_TAB_MIN) {
        // adds all fields to hash table (including the one we just added)
        rehash();
    }

    return getField(pos).val;
}
template Value& DocumentStorage::appendField<StringData>(StringData, ValueElement::Kind);
template Value& DocumentStorage::appendField<HashedFieldName>(HashedFieldName, ValueElement::Kind);

// Call after adding field to _fields and increasing _numFields
template <typename T>
void DocumentStorage::addFieldToHashTable(T field, Position pos) {
    ValueElement& elem = getField(pos);
    elem.nextCollision = Position();

    const unsigned bucket = bucketForKey(field);

    Position* posPtr = &_hashTab[bucket];
    while (posPtr->found()) {
        // collision: walk links and add new to end
        posPtr = &getField(*posPtr).nextCollision;
    }
    *posPtr = Position(pos.index);
}

void DocumentStorage::alloc(unsigned newSize) {
    const bool firstAlloc = !_cache;
    const bool doingRehash = needRehash();
    const size_t oldCapacity = _cacheEnd - _cache;

    // make new bucket count big enough
    while (needRehash() || hashTabBuckets() < HASH_TAB_INIT_SIZE)
        _hashTabMask = hashTabBuckets() * 2 - 1;

    // only allocate power-of-two sized space > 128 bytes
    size_t capacity = 128;
    while (capacity < newSize + hashTabBytes())
        capacity *= 2;

    uassert(16490, "Tried to make oversized document", capacity <= size_t(BufferMaxSize));

    std::unique_ptr<char[]> oldBuf(_cache);
    _cache = new char[capacity];
    _cacheEnd = _cache + capacity - hashTabBytes();

    if (!firstAlloc) {
        // This just copies the elements
        memcpy(_cache, oldBuf.get(), _usedBytes);

        if (_numFields >= HASH_TAB_MIN) {
            // if we were hashing, deal with the hash table
            if (doingRehash) {
                rehash();
            } else {
                // no rehash needed so just slide table down to new position
                memcpy(_hashTab, oldBuf.get() + oldCapacity, hashTabBytes());
            }
        }
    }
}

void DocumentStorage::reserveFields(size_t expectedFields) {
    fassert(16487, !_cache);

    unsigned buckets = HASH_TAB_INIT_SIZE;
    while (buckets < expectedFields)
        buckets *= 2;
    _hashTabMask = buckets - 1;

    // Using expectedFields+1 to allow space for long field names
    const size_t newSize = (expectedFields + 1) * ValueElement::align(sizeof(ValueElement));

    uassert(16491, "Tried to make oversized document", newSize <= size_t(BufferMaxSize));

    _cache = new char[newSize + hashTabBytes()];
    _cacheEnd = _cache + newSize;
}

intrusive_ptr<DocumentStorage> DocumentStorage::clone() const {
    auto out =
        make_intrusive<DocumentStorage>(_bson, _stripMetadata, _modified, _numBytesFromBSONInCache);

    if (_cache) {
        // Make a copy of the buffer with the fields.
        // It is very important that the positions of each field are the same after cloning.
        const size_t bufferBytes = allocatedBytes();
        out->_cache = new char[bufferBytes];
        out->_cacheEnd = out->_cache + (_cacheEnd - _cache);
        memcpy(out->_cache, _cache, bufferBytes);

        out->_hashTabMask = _hashTabMask;
        out->_usedBytes = _usedBytes;
        out->_numFields = _numFields;

        dassert(out->allocatedBytes() == bufferBytes);

        // Tell values that they have been memcpyed (updates ref counts)
        for (auto it = out->iteratorCacheOnly(); !it.atEnd(); it.advance()) {
            it->val.memcpyed();
        }
    } else {
        // If we don't have a buffer, these fields should still be in their initial state.
        dassert(out->_hashTabMask == _hashTabMask);
        dassert(out->_usedBytes == _usedBytes);
        dassert(out->_numFields == _numFields);
    }

    out->_haveLazyLoadedMetadata = _haveLazyLoadedMetadata;
    out->_metadataFields = _metadataFields;

    return out;
}

size_t DocumentStorage::getMetadataApproximateSize() const {
    return _metadataFields.getApproximateSize();
}

DocumentStorage::~DocumentStorage() {
    std::unique_ptr<char[]> deleteBufferAtScopeEnd(_cache);

    for (auto it = iteratorCacheOnly(); !it.atEnd(); it.advance()) {
        it->val.~Value();  // explicit destructor call
    }
}

void DocumentStorage::reset(const BSONObj& bson, bool stripMetadata) {
    _bson = bson;
    _numBytesFromBSONInCache = 0;
    _stripMetadata = stripMetadata;
    _modified = false;

    // Clean cache.
    for (auto it = iteratorCacheOnly(); !it.atEnd(); it.advance()) {
        it->val.~Value();  // explicit destructor call
    }

    _cacheEnd = _cache;
    _usedBytes = 0;
    _numFields = 0;
    _hashTabMask = 0;

    // Clean metadata.
    _metadataFields = DocumentMetadataFields{};
}

void DocumentStorage::fillCache() const {
    for (DocumentStorageIterator it = iterator(); !it.atEnd(); it.advance()) {
        it->val.fillCache();
    }
}

void DocumentStorage::loadLazyMetadata() const {
    if (_haveLazyLoadedMetadata) {
        return;
    }

    BSONObjIterator it(_bson);
    while (it.more()) {
        BSONElement elem(it.next());
        auto fieldName = elem.fieldNameStringData();
        if (fieldName[0] == '$') {
            if (fieldName == Document::metaFieldTextScore) {
                _metadataFields.setTextScore(elem.Double());
            } else if (fieldName == Document::metaFieldSearchScore) {
                _metadataFields.setSearchScore(elem.Double());
            } else if (fieldName == Document::metaFieldSearchHighlights) {
                _metadataFields.setSearchHighlights(Value(elem));
            } else if (fieldName == Document::metaFieldRandVal) {
                _metadataFields.setRandVal(elem.Double());
            } else if (fieldName == Document::metaFieldSortKey) {
                auto bsonSortKey = elem.Obj();

                // If the sort key has exactly one field, we say it is a "single element key."
                BSONObjIterator sortKeyIt(bsonSortKey);
                uassert(31282, "Empty sort key in metadata", sortKeyIt.more());
                bool isSingleElementKey = !(++sortKeyIt).more();

                _metadataFields.setSortKey(
                    DocumentMetadataFields::deserializeSortKey(isSingleElementKey, bsonSortKey),
                    isSingleElementKey);
            } else if (fieldName == Document::metaFieldGeoNearDistance) {
                _metadataFields.setGeoNearDistance(elem.Double());
            } else if (fieldName == Document::metaFieldGeoNearPoint) {
                Value val;
                if (elem.type() == BSONType::Array) {
                    val = Value(BSONArray(elem.embeddedObject()));
                } else {
                    invariant(elem.type() == BSONType::Object);
                    val = Value(elem.embeddedObject());
                }

                _metadataFields.setGeoNearPoint(val);
            } else if (fieldName == Document::metaFieldIndexKey) {
                _metadataFields.setIndexKey(elem.Obj());
            } else if (fieldName == Document::metaFieldSearchScoreDetails) {
                _metadataFields.setSearchScoreDetails(elem.Obj());
            }
        }
    }

    _haveLazyLoadedMetadata = true;
}

Document::Document(const BSONObj& bson) {
    MutableDocument md;
    md.newStorageWithBson(bson, false);

    *this = md.freeze();
}

Document::Document(std::initializer_list<std::pair<StringData, ImplicitValue>> initializerList) {
    MutableDocument mutableDoc(initializerList.size());

    for (auto&& pair : initializerList) {
        mutableDoc.addField(pair.first, pair.second);
    }

    *this = mutableDoc.freeze();
}

Document::Document(std::vector<std::pair<StringData, Value>> fields) {
    MutableDocument mutableDoc(fields.size());
    for (auto&& pair : fields)
        mutableDoc.addField(pair.first, pair.second);
    *this = mutableDoc.freeze();
}

BSONObjBuilder& operator<<(BSONObjBuilderValueStream& builder, const Document& doc) {
    BSONObjBuilder subobj(builder.subobjStart());
    doc.toBson(&subobj);
    subobj.doneFast();
    return builder.builder();
}

void Document::toBson(BSONObjBuilder* builder, size_t recursionLevel) const {
    uassert(ErrorCodes::Overflow,
            str::stream() << "cannot convert document to BSON because it exceeds the limit of "
                          << BSONDepth::getMaxAllowableDepth() << " levels of nesting",
            recursionLevel <= BSONDepth::getMaxAllowableDepth());

    for (DocumentStorageIterator it = storage().iterator(); !it.atEnd(); it.advance()) {
        if (auto cached = it.cachedValue()) {
            cached->val.addToBsonObj(builder, cached->nameSD(), recursionLevel);
        } else {
            builder->append(*it.bsonIter());
        }
    }
}

BSONObj Document::toBson() const {
    if (!storage().isModified() && !storage().stripMetadata()) {
        return storage().bsonObj();
    }

    BSONObjBuilder bb;
    toBson(&bb);
    return bb.obj();
}

boost::optional<BSONObj> Document::toBsonIfTriviallyConvertible() const {
    if (!storage().isModified() && !storage().stripMetadata()) {
        return storage().bsonObj();
    } else {
        return boost::none;
    }
}

constexpr StringData Document::metaFieldTextScore;
constexpr StringData Document::metaFieldRandVal;
constexpr StringData Document::metaFieldSortKey;
constexpr StringData Document::metaFieldGeoNearDistance;
constexpr StringData Document::metaFieldGeoNearPoint;
constexpr StringData Document::metaFieldSearchScore;
constexpr StringData Document::metaFieldSearchHighlights;
constexpr StringData Document::metaFieldSearchScoreDetails;

BSONObj Document::toBsonWithMetaData() const {
    BSONObjBuilder bb;
    toBson(&bb);
    if (!metadata()) {
        return bb.obj();
    }

    if (metadata().hasTextScore())
        bb.append(metaFieldTextScore, metadata().getTextScore());
    if (metadata().hasRandVal())
        bb.append(metaFieldRandVal, metadata().getRandVal());
    if (metadata().hasSortKey())
        bb.append(metaFieldSortKey,
                  DocumentMetadataFields::serializeSortKey(metadata().isSingleElementKey(),
                                                           metadata().getSortKey()));
    if (metadata().hasGeoNearDistance())
        bb.append(metaFieldGeoNearDistance, metadata().getGeoNearDistance());
    if (metadata().hasGeoNearPoint())
        metadata().getGeoNearPoint().addToBsonObj(&bb, metaFieldGeoNearPoint);
    if (metadata().hasSearchScore())
        bb.append(metaFieldSearchScore, metadata().getSearchScore());
    if (metadata().hasSearchHighlights())
        metadata().getSearchHighlights().addToBsonObj(&bb, metaFieldSearchHighlights);
    if (metadata().hasIndexKey())
        bb.append(metaFieldIndexKey, metadata().getIndexKey());
    if (metadata().hasSearchScoreDetails())
        bb.append(metaFieldSearchScoreDetails, metadata().getSearchScoreDetails());
    return bb.obj();
}

Document Document::fromBsonWithMetaData(const BSONObj& bson) {
    MutableDocument md;
    md.newStorageWithBson(bson, true);

    return md.freeze();
}

Document Document::getOwned() const& {
    if (isOwned()) {
        return *this;
    } else {
        MutableDocument md(*this);
        md.makeOwned();
        return md.freeze();
    }
}

Document Document::getOwned() && {
    if (isOwned()) {
        return std::move(*this);
    } else {
        MutableDocument md(std::move(*this));
        md.makeOwned();
        return md.freeze();
    }
}

MutableDocument::MutableDocument(size_t expectedFields)
    : _storageHolder(nullptr), _storage(_storageHolder) {
    if (expectedFields) {
        storage().reserveFields(expectedFields);
    }
}

MutableValue MutableDocument::getNestedFieldHelper(const FieldPath& dottedField, size_t level) {
    if (level == dottedField.getPathLength() - 1) {
        return getField(dottedField.getFieldName(level));
    } else {
        MutableDocument nested(getFieldNonLeaf(dottedField.getFieldName(level)));
        return nested.getNestedFieldHelper(dottedField, level + 1);
    }
}

MutableValue MutableDocument::getNestedField(const FieldPath& dottedField) {
    fassert(16601, dottedField.getPathLength());
    assertFieldPathLengthOK(dottedField);
    return getNestedFieldHelper(dottedField, 0);
}

MutableValue MutableDocument::getNestedFieldHelper(const vector<Position>& positions,
                                                   size_t level) {
    if (level == positions.size() - 1) {
        return getField(positions[level]);
    } else {
        MutableDocument nested(getField(positions[level]));
        return nested.getNestedFieldHelper(positions, level + 1);
    }
}

MutableValue MutableDocument::getNestedField(const vector<Position>& positions) {
    fassert(16488, !positions.empty());
    assertFieldPathLengthOK(positions);
    return getNestedFieldHelper(positions, 0);
}


stdx::variant<BSONElement, Value, Document::TraversesArrayTag, stdx::monostate>
Document::getNestedFieldNonCachingHelper(const FieldPath& dottedField, size_t level) const {
    if (!_storage) {
        return stdx::monostate{};
    }

    StringData fieldName = dottedField.getFieldName(level);
    auto bsonEltOrValue = _storage->getFieldNonCaching(fieldName);

    if (stdx::holds_alternative<BSONElement>(bsonEltOrValue)) {
        return getNestedFieldHelperBSON(
            stdx::get<BSONElement>(bsonEltOrValue), dottedField, level + 1);
    }

    const Value& val = stdx::get<Value>(bsonEltOrValue);

    if (level + 1 == dottedField.getPathLength()) {
        if (val.missing()) {
            return stdx::monostate{};
        } else {
            return val;
        }
    }

    if (val.getType() == BSONType::Array) {
        return Document::TraversesArrayTag{};
    } else if (val.getType() == BSONType::Object) {
        return val.getDocument().getNestedFieldNonCachingHelper(dottedField, level + 1);
    }

    // The path extends beyond a scalar, so it does not exist.
    return stdx::monostate{};
}

stdx::variant<BSONElement, Value, Document::TraversesArrayTag, stdx::monostate>
Document::getNestedFieldNonCaching(const FieldPath& dottedField) const {
    return getNestedFieldNonCachingHelper(dottedField, 0);
}

static Value getNestedFieldHelper(const Document& doc,
                                  const FieldPath& fieldNames,
                                  vector<Position>* positions,
                                  size_t level) {
    const auto fieldName = fieldNames.getFieldName(level);
    const Position pos = doc.positionOf(fieldName);

    if (!pos.found())
        return Value();

    if (positions)
        positions->push_back(pos);

    if (level == fieldNames.getPathLength() - 1)
        return doc.getField(pos);

    Value val = doc.getField(pos);
    if (val.getType() != Object)
        return Value();

    return getNestedFieldHelper(val.getDocument(), fieldNames, positions, level + 1);
}

const Value Document::getNestedField(const FieldPath& path, vector<Position>* positions) const {
    fassert(16489, path.getPathLength());
    assertFieldPathLengthOK(path);
    return getNestedFieldHelper(*this, path, positions, 0);
}

size_t Document::getApproximateSizeWithoutBackingBSON() const {
    size_t size = sizeof(Document);
    if (!_storage)
        return size;

    size += sizeof(DocumentStorage);
    size += storage().allocatedBytes();

    for (auto it = storage().iteratorCacheOnly(); !it.atEnd(); it.advance()) {
        size += it->val.getApproximateSize();
        size -= sizeof(Value);  // already accounted for above
    }

    // The metadata also occupies space in the document storage that's pre-allocated.
    size += storage().getMetadataApproximateSize();

    return size;
}

size_t Document::getApproximateSize() const {
    return getApproximateSizeWithoutBackingBSON() + storage().bsonObjSize();
}

size_t Document::memUsageForSorter() const {
    return getApproximateSizeWithoutBackingBSON() + storage().nonCachedBsonObjSize();
}

void Document::hash_combine(size_t& seed,
                            const StringData::ComparatorInterface* stringComparator) const {
    for (DocumentStorageIterator it = storage().iterator(); !it.atEnd(); it.advance()) {
        StringData name = it->nameSD();
        boost::hash_range(seed, name.rawData(), name.rawData() + name.size());
        it->val.hash_combine(seed, stringComparator);
    }
}

int Document::compare(const Document& rL,
                      const Document& rR,
                      const StringData::ComparatorInterface* stringComparator) {

    if (&rL.storage() == &rR.storage()) {
        // If the storage is the same (shared between the documents) then the documents must be
        // equal.
        return 0;
    }
    DocumentStorageIterator lIt = rL.storage().iterator();
    DocumentStorageIterator rIt = rR.storage().iterator();

    while (true) {
        if (lIt.atEnd()) {
            if (rIt.atEnd())
                return 0;  // documents are the same length

            return -1;  // left document is shorter
        }

        if (rIt.atEnd())
            return 1;  // right document is shorter

        const ValueElement& rField = rIt.get();
        const ValueElement& lField = lIt.get();

        // For compatibility with BSONObj::woCompare() consider the canonical type of values
        // before considerting their names.
        if (lField.val.getType() != rField.val.getType()) {
            const int rCType = canonicalizeBSONType(rField.val.getType());
            const int lCType = canonicalizeBSONType(lField.val.getType());
            if (lCType != rCType)
                return lCType < rCType ? -1 : 1;
        }

        const int nameCmp = lField.nameSD().compare(rField.nameSD());
        if (nameCmp)
            return nameCmp;  // field names are unequal

        const int valueCmp = Value::compare(lField.val, rField.val, stringComparator);
        if (valueCmp)
            return valueCmp;  // fields are unequal

        rIt.advance();
        lIt.advance();
    }
}

string Document::toString() const {
    if (empty())
        return "{}";

    StringBuilder out;
    const char* prefix = "{";

    for (DocumentStorageIterator it = storage().iterator(); !it.atEnd(); it.advance()) {
        out << prefix << it->nameSD() << ": " << it->val.toString();
        prefix = ", ";
    }
    out << '}';

    return out.str();
}

void Document::serializeForSorter(BufBuilder& buf) const {
    const size_t numElems = computeSize();
    buf.appendNum(static_cast<int>(numElems));

    for (DocumentStorageIterator it = storage().iterator(); !it.atEnd(); it.advance()) {
        buf.appendStr(it->nameSD(), /*NUL byte*/ true);
        it->val.serializeForSorter(buf);
    }

    metadata().serializeForSorter(buf);
}

Document Document::deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&) {
    const int numElems = buf.read<LittleEndian<int>>();
    MutableDocument doc(numElems);
    for (int i = 0; i < numElems; i++) {
        StringData name = buf.readCStr();
        doc.addField(name, Value::deserializeForSorter(buf, Value::SorterDeserializeSettings()));
    }

    DocumentMetadataFields::deserializeForSorter(buf, &doc.metadata());

    return doc.freeze();
}
}  // namespace mongo
