/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pch.h"

#include "mongo/db/pipeline/document.h"

#include <boost/functional/hash.hpp>

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
    using namespace mongoutils;

    Position DocumentStorage::findField(StringData requested) const {
        if (_numFields >= HASH_TAB_MIN) { // hash lookup
            const unsigned bucket = bucketForKey(requested);

            Position pos = _hashTab[bucket];
            while (pos.found()) {
                const ValueElement& elem = getField(pos);
                if (requested == elem.nameSD())
                    return pos;

                // possible collision
                pos = elem.nextCollision;
            }
        }
        else if (_numFields) { // linear scan
            for (DocumentStorageIterator it = iteratorAll(); !it.atEnd(); it.advance()) {
                if (size_t(it->nameLen) == requested.size()
                    && requested == it->nameSD()) {
                    return it.position();
                }
            }
        }

        // if we got here, there's no such field
        return Position();
    }

    Value& DocumentStorage::appendField(StringData name) {
        Position pos = getNextPosition();
        const int nameSize = name.size();

        // these are the same for everyone
        const Position nextCollision;
        const Value value;

        // Make room for new field (and padding at end for alignment)
        const unsigned newUsed = ValueElement::align(_usedBytes + sizeof(ValueElement) + nameSize);
        if (_buffer + newUsed > _bufferEnd)
            alloc(newUsed);
        _usedBytes = newUsed;

        // Append structure of a ValueElement
        char* dest = _buffer + pos.index; // must be after alloc since it changes _buffer
#define append(x) memcpy(dest, &(x), sizeof(x)); dest += sizeof(x)
        append(value);
        append(nextCollision);
        append(nameSize);
        name.copyTo( dest, true );
        // Padding for alignment handled above
#undef append

        // Make sure next field starts where we expect it
        fassert(16486, getField(pos).next()->ptr() == _buffer + _usedBytes);

        _numFields++;

        if (_numFields > HASH_TAB_MIN) {
            addFieldToHashTable(pos);
        }
        else if (_numFields == HASH_TAB_MIN) {
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
        const bool firstAlloc = !_buffer;
        const bool doingRehash = needRehash();
        const size_t oldCapacity = _bufferEnd - _buffer;

        // make new bucket count big enough
        while (needRehash() || hashTabBuckets() < HASH_TAB_INIT_SIZE)
            _hashTabMask = hashTabBuckets()*2 - 1;

        // only allocate power-of-two sized space > 128 bytes
        size_t capacity = 128;
        while (capacity < newSize + hashTabBytes())
            capacity *= 2;

        uassert(16490, "Tried to make oversized document",
                capacity <= size_t(BufferMaxSize));

        boost::scoped_array<char> oldBuf(_buffer);
        _buffer = new char[capacity];
        _bufferEnd = _buffer + capacity - hashTabBytes();

        if (!firstAlloc) {
            // This just copies the elements
            memcpy(_buffer, oldBuf.get(), _usedBytes);

            if (_numFields >= HASH_TAB_MIN) {
                // if we were hashing, deal with the hash table
                if (doingRehash) {
                    rehash();
                }
                else {
                    // no rehash needed so just slide table down to new position
                    memcpy(_hashTab, oldBuf.get() + oldCapacity, hashTabBytes());
                }
            }
        }
    }

    void DocumentStorage::reserveFields(size_t expectedFields) {
        fassert(16487, !_buffer);

        unsigned buckets = HASH_TAB_INIT_SIZE;
        while (buckets < expectedFields)
            buckets *= 2;
        _hashTabMask = buckets - 1;

        // Using expectedFields+1 to allow space for long field names
        const size_t newSize = (expectedFields+1) * ValueElement::align(sizeof(ValueElement));

        uassert(16491, "Tried to make oversized document",
                newSize <= size_t(BufferMaxSize));

        _buffer = new char[newSize + hashTabBytes()];
        _bufferEnd = _buffer + newSize;
    }

    intrusive_ptr<DocumentStorage> DocumentStorage::clone() const {
        intrusive_ptr<DocumentStorage> out (new DocumentStorage());

        // Make a copy of the buffer.
        // It is very important that the positions of each field are the same after cloning.
        const size_t bufferBytes = (_bufferEnd + hashTabBytes()) - _buffer;
        out->_buffer = new char[bufferBytes];
        out->_bufferEnd = out->_buffer + (_bufferEnd - _buffer);
        memcpy(out->_buffer, _buffer, bufferBytes);

        // Copy remaining fields
        out->_usedBytes = _usedBytes;
        out->_numFields = _numFields;
        out->_hashTabMask = _hashTabMask;

        // Tell values that they have been memcpyed (updates ref counts)
        for (DocumentStorageIterator it = out->iteratorAll(); !it.atEnd(); it.advance()) {
            it->val.memcpyed();
        }

        return out;
    }

    DocumentStorage::~DocumentStorage() {
        boost::scoped_array<char> deleteBufferAtScopeEnd (_buffer);

        for (DocumentStorageIterator it = iteratorAll(); !it.atEnd(); it.advance()) {
            it->val.~Value(); // explicit destructor call
        }
    }

    Document::Document(const BSONObj& bson) {
        MutableDocument md(bson.nFields());

        BSONObjIterator it(bson);
        while(it.more()) {
            BSONElement bsonElement(it.next());
            md.addField(bsonElement.fieldName(), Value(bsonElement));
        }

        *this = md.freeze();
    }

    BSONObjBuilder& operator << (BSONObjBuilderValueStream& builder, const Document& doc) {
        BSONObjBuilder subobj(builder.subobjStart());
        doc.toBson(&subobj);
        subobj.doneFast();
        return builder.builder();
    }

    void Document::toBson(BSONObjBuilder* pBuilder) const {
        for (DocumentStorageIterator it = storage().iterator(); !it.atEnd(); it.advance()) {
            *pBuilder << it->name << it->val;
        }
    }

    MutableDocument::MutableDocument(size_t expectedFields)
        : _storageHolder(NULL)
        , _storage(_storageHolder)
    {
        if (expectedFields) {
            storage().reserveFields(expectedFields);
        }
    }

    MutableValue MutableDocument::getNestedFieldHelper(MutableDocument& md,
                                                       const vector<Position>& positions,
                                                       size_t level) {
        fassert(16488, !positions.empty());

        if (level == positions.size()-1) {
            return md[positions[level]];
        }
        else {
            MutableDocument nested (md[positions[level]]);
            return getNestedFieldHelper(nested, positions, level+1);
        }
    }

    MutableValue MutableDocument::getNestedField(const vector<Position>& positions) {
        return getNestedFieldHelper(*this, positions, 0);
    }

    static Value getNestedFieldHelper(const Document& doc,
                                      const FieldPath& fieldNames,
                                      vector<Position>* positions,
                                      size_t level) {

        fassert(16489, fieldNames.getPathLength());

        const string& fieldName = fieldNames.getFieldName(level);
        const Position pos = doc.positionOf(fieldName);

        if (!pos.found())
            return Value();

        if (positions)
            positions->push_back(pos);

        if (level == fieldNames.getPathLength()-1)
            return doc.getField(pos);

        Value val = doc.getField(pos);
        if (val.getType() != Object)
            return Value();

        return getNestedFieldHelper(val.getDocument(), fieldNames, positions, level+1);
    }

    const Value Document::getNestedField(const FieldPath& fieldNames,
                                         vector<Position>* positions) const {
        return getNestedFieldHelper(*this, fieldNames, positions, 0);
    }

    size_t Document::getApproximateSize() const {
        if (!_storage)
            return 0; // we've allocated no memory

        size_t size = sizeof(DocumentStorage);
        size += storage().allocatedBytes();

        for (DocumentStorageIterator it = storage().iterator(); !it.atEnd(); it.advance()) {
            size += it->val.getApproximateSize();
            size -= sizeof(Value); // already accounted for above
        }

        return size;
    }

    void Document::hash_combine(size_t &seed) const {
        for (DocumentStorageIterator it = storage().iterator(); !it.atEnd(); it.advance()) {
            StringData name = it->nameSD();
            boost::hash_range(seed, name.rawData(), name.rawData() + name.size());
            it->val.hash_combine(seed);
        }
    }

    int Document::compare(const Document& rL, const Document& rR) {
        DocumentStorageIterator lIt = rL.storage().iterator();
        DocumentStorageIterator rIt = rR.storage().iterator();

        while (true) {
            if (lIt.atEnd()) {
                if (rIt.atEnd())
                    return 0; // documents are the same length

                return -1; // left document is shorter
            }

            if (rIt.atEnd())
                return 1; // right document is shorter

            const ValueElement& rField = rIt.get();
            const ValueElement& lField = lIt.get();

            const int nameCmp = strcmp(lField.name, rField.name);
            if (nameCmp)
                return nameCmp; // field names are unequal

            const int valueCmp = Value::compare(lField.val, rField.val);
            if (valueCmp)
                return valueCmp; // fields are unequal

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
            out << prefix << it->name << ": " << it->val.toString();
            prefix = ", ";
        }
        out << '}';

        return out.str();
    }
}
