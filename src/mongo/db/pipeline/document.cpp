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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document.h"

#include <boost/functional/hash.hpp>

#include "mongo/bson/bson_depth.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/str.h"

namespace mongo {
using boost::intrusive_ptr;
using std::string;
using std::vector;

const DocumentStorage DocumentStorage::kEmptyDoc;

const StringDataSet Document::allMetadataFieldNames{Document::metaFieldTextScore,
                                                    Document::metaFieldRandVal,
                                                    Document::metaFieldSortKey,
                                                    Document::metaFieldGeoNearDistance,
                                                    Document::metaFieldGeoNearPoint,
                                                    Document::metaFieldSearchScore,
                                                    Document::metaFieldSearchHighlights};

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

Position DocumentStorage::findFieldInCache(StringData requested) const {
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

Position DocumentStorage::findField(StringData requested, LookupPolicy policy) const {
    if (auto pos = findFieldInCache(requested); pos.found() || policy == LookupPolicy::kCacheOnly) {
        return pos;
    }

    while (_bsonIt.more()) {
        BSONElement bsonElement(_bsonIt.next());
        // In order to avoid repeatedly scanning the BSON we were constructed from, we'll bring in a
        // copy of every value we encounter while searching here. That way the next time we search
        // we won't have to reconsider elements we've already examined and can avoid an O(N^2) worst
        // case performance.
        auto pos = const_cast<DocumentStorage*>(this)->constructInCache(bsonElement);
        if (requested == bsonElement.fieldNameStringData()) {
            return pos;
        }
    }

    // if we got here, there's no such field
    return Position();
}

Position DocumentStorage::constructInCache(const BSONElement& elem) {
    auto savedModified = _modified;
    auto pos = getNextPosition();
    const auto fieldName = elem.fieldNameStringData();
    appendField(fieldName, ValueElement::Kind::kCached) = Value(elem);
    _modified = savedModified;

    return pos;
}

Value& DocumentStorage::appendField(StringData name, ValueElement::Kind kind) {
    Position pos = getNextPosition();
    const int nameSize = name.size();

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
    name.copyTo(dest, true);
// Padding for alignment handled above
#undef append

    // Make sure next field starts where we expect it
    fassert(16486, getField(pos).next()->ptr() == _cache + _usedBytes);

    _numFields++;

    if (_numFields > HASH_TAB_MIN) {
        addFieldToHashTable(pos);
    } else if (_numFields == HASH_TAB_MIN) {
        // adds all fields to hash table (including the one we just added)
        rehash();
    }

    return getField(pos).val;
}

// Call after adding field to _fields and increasing _numFields
void DocumentStorage::addFieldToHashTable(Position pos) {
    ValueElement& elem = getField(pos);
    elem.nextCollision = Position();

    const unsigned bucket = bucketForKey(elem.nameSD());

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
    auto out = make_intrusive<DocumentStorage>(_bson, _stripMetadata, _modified);

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

    // Copy metadata
    if (_metadataFields) {
        out->_metadataFields = std::make_unique<MetadataFields>(*_metadataFields);
    }

    return out;
}

MetadataFields::MetadataFields(const MetadataFields& other) {
    _metaFields = other._metaFields;
    _textScore = other._textScore;
    _randVal = other._randVal;
    _sortKey = other._sortKey.getOwned();
    _geoNearDistance = other._geoNearDistance;
    _geoNearPoint = other._geoNearPoint.getOwned();
    _searchScore = other._searchScore;
    _searchHighlights = other._searchHighlights;
}

size_t MetadataFields::getApproximateSize() const {
    size_t size = sizeof(MetadataFields);

    // Count the "deep" portion of the metadata values.
    size += _sortKey.objsize();
    size += _geoNearPoint.getApproximateSize();
    // Size of Value is double counted - once in sizeof(MetadataFields) and once in
    // getApproximateSize()
    size -= sizeof(_geoNearPoint);
    size += _searchHighlights.getApproximateSize();
    size -= sizeof(_searchHighlights);

    return size;
}

size_t DocumentStorage::getMetadataApproximateSize() const {
    if (!_metadataFields) {
        return 0;
    }

    return _metadataFields->getApproximateSize();
}

DocumentStorage::~DocumentStorage() {
    std::unique_ptr<char[]> deleteBufferAtScopeEnd(_cache);

    for (auto it = iteratorCacheOnly(); !it.atEnd(); it.advance()) {
        it->val.~Value();  // explicit destructor call
    }
}

void DocumentStorage::loadLazyMetadata() const {
    if (_metadataFields) {
        return;
    }

    _metadataFields = std::make_unique<MetadataFields>();

    BSONObjIterator it(_bson);
    while (it.more()) {
        BSONElement elem(it.next());
        auto fieldName = elem.fieldNameStringData();
        if (fieldName[0] == '$') {
            if (fieldName == Document::metaFieldTextScore) {
                _metadataFields->setTextScore(elem.Double());
            } else if (fieldName == Document::metaFieldSearchScore) {
                _metadataFields->setSearchScore(elem.Double());
            } else if (fieldName == Document::metaFieldSearchHighlights) {
                _metadataFields->setSearchHighlights(Value(elem));
            } else if (fieldName == Document::metaFieldRandVal) {
                _metadataFields->setRandMetaField(elem.Double());
            } else if (fieldName == Document::metaFieldSortKey) {
                _metadataFields->setSortKeyMetaField(elem.Obj());
            } else if (fieldName == Document::metaFieldGeoNearDistance) {
                _metadataFields->setGeoNearDistance(elem.Double());
            } else if (fieldName == Document::metaFieldGeoNearPoint) {
                Value val;
                if (elem.type() == BSONType::Array) {
                    val = Value(BSONArray(elem.embeddedObject()));
                } else {
                    invariant(elem.type() == BSONType::Object);
                    val = Value(elem.embeddedObject());
                }

                _metadataFields->setGeoNearPoint(val);
            }
        }
    }
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

BSONObjBuilder& operator<<(BSONObjBuilderValueStream& builder, const Document& doc) {
    BSONObjBuilder subobj(builder.subobjStart());
    doc.toBson(&subobj);
    subobj.doneFast();
    return builder.builder();
}

void Document::toBson(BSONObjBuilder* builder, size_t recursionLevel) const {
    uassert(ErrorCodes::Overflow,
            str::stream() << "cannot convert document to BSON because it exceeds the limit of "
                          << BSONDepth::getMaxAllowableDepth()
                          << " levels of nesting",
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

constexpr StringData Document::metaFieldTextScore;
constexpr StringData Document::metaFieldRandVal;
constexpr StringData Document::metaFieldSortKey;
constexpr StringData Document::metaFieldGeoNearDistance;
constexpr StringData Document::metaFieldGeoNearPoint;
constexpr StringData Document::metaFieldSearchScore;
constexpr StringData Document::metaFieldSearchHighlights;

BSONObj Document::toBsonWithMetaData() const {
    BSONObjBuilder bb;
    toBson(&bb);
    if (hasTextScore())
        bb.append(metaFieldTextScore, getTextScore());
    if (hasRandMetaField())
        bb.append(metaFieldRandVal, getRandMetaField());
    if (hasSortKeyMetaField())
        bb.append(metaFieldSortKey, getSortKeyMetaField());
    if (hasGeoNearDistance())
        bb.append(metaFieldGeoNearDistance, getGeoNearDistance());
    if (hasGeoNearPoint())
        getGeoNearPoint().addToBsonObj(&bb, metaFieldGeoNearPoint);
    if (hasSearchScore())
        bb.append(metaFieldSearchScore, getSearchScore());
    if (hasSearchHighlights())
        getSearchHighlights().addToBsonObj(&bb, metaFieldSearchHighlights);
    return bb.obj();
}

Document Document::fromBsonWithMetaData(const BSONObj& bson) {
    MutableDocument md;
    md.newStorageWithBson(bson, true);

    return md.freeze();
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
    return getNestedFieldHelper(positions, 0);
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
    return getNestedFieldHelper(*this, path, positions, 0);
}

size_t Document::getApproximateSize() const {
    if (!_storage)
        return 0;  // we've allocated no memory

    size_t size = sizeof(DocumentStorage);
    size += storage().allocatedBytes();

    for (auto it = storage().iteratorCacheOnly(); !it.atEnd(); it.advance()) {
        size += it->val.getApproximateSize();
        size -= sizeof(Value);  // already accounted for above
    }

    // The metadata also occupies space in the document storage that's pre-allocated.
    size += getMetadataApproximateSize();
    size += storage().bsonObjSize();

    return size;
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
    const int numElems = size();
    buf.appendNum(numElems);

    for (DocumentStorageIterator it = storage().iterator(); !it.atEnd(); it.advance()) {
        buf.appendStr(it->nameSD(), /*NUL byte*/ true);
        it->val.serializeForSorter(buf);
    }

    if (hasTextScore()) {
        buf.appendNum(char(MetaType::TEXT_SCORE + 1));
        buf.appendNum(getTextScore());
    }
    if (hasRandMetaField()) {
        buf.appendNum(char(MetaType::RAND_VAL + 1));
        buf.appendNum(getRandMetaField());
    }
    if (hasSortKeyMetaField()) {
        buf.appendNum(char(MetaType::SORT_KEY + 1));
        getSortKeyMetaField().appendSelfToBufBuilder(buf);
    }
    if (hasSearchScore()) {
        buf.appendNum(char(MetaType::SEARCH_SCORE + 1));
        buf.appendNum(getSearchScore());
    }
    if (hasSearchHighlights()) {
        buf.appendNum(char(MetaType::SEARCH_HIGHLIGHTS + 1));
        getSearchHighlights().serializeForSorter(buf);
    }
    buf.appendNum(char(0));
}

Document Document::deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&) {
    const int numElems = buf.read<LittleEndian<int>>();
    MutableDocument doc(numElems);
    for (int i = 0; i < numElems; i++) {
        StringData name = buf.readCStr();
        doc.addField(name, Value::deserializeForSorter(buf, Value::SorterDeserializeSettings()));
    }

    while (char marker = buf.read<char>()) {
        if (marker == char(MetaType::TEXT_SCORE) + 1) {
            doc.setTextScore(buf.read<LittleEndian<double>>());
        } else if (marker == char(MetaType::RAND_VAL) + 1) {
            doc.setRandMetaField(buf.read<LittleEndian<double>>());
        } else if (marker == char(MetaType::SORT_KEY) + 1) {
            doc.setSortKeyMetaField(
                BSONObj::deserializeForSorter(buf, BSONObj::SorterDeserializeSettings()));
        } else if (marker == char(MetaType::SEARCH_SCORE) + 1) {
            doc.setSearchScore(buf.read<LittleEndian<double>>());
        } else if (marker == char(MetaType::SEARCH_HIGHLIGHTS) + 1) {
            doc.setSearchHighlights(
                Value::deserializeForSorter(buf, Value::SorterDeserializeSettings()));
        } else {
            uasserted(28744, "Unrecognized marker, unable to deserialize buffer");
        }
    }

    return doc.freeze();
}
}
