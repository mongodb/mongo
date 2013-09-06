/**
 * Copyright 2011 (c) 10gen Inc.
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
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#pragma once

#include "mongo/db/pipeline/document_internal.h"

#include <boost/functional/hash.hpp>
#include "mongo/bson/util/builder.h"

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
     *  A Document can be treated as a const map<string, const Value> that is
     *  very cheap to copy and is Assignable.  Therefore, it is acceptable to
     *  pass and return by Value. Note that the data in a Document is
     *  immutable, but you can replace a Document instance with assignment.
     *
     *  See Also: Value class in Value.h
     */
    class Document {
    public:

        /// Empty Document (does no allocation)
        Document() {}

        /// Create a new Document deep-converted from the given BSONObj.
        explicit Document(const BSONObj& bson);

        void swap(Document& rhs) { _storage.swap(rhs._storage); }

        /// Look up a field by key name. Returns Value() if no such field. O(1)
        const Value operator[] (StringData key) const { return getField(key); }
        const Value getField(StringData key) const { return storage().getField(key); }

        /// Look up a field by Position. See positionOf and getNestedField.
        const Value operator[] (Position pos) const { return getField(pos); }
        const Value getField(Position pos) const { return storage().getField(pos).val; }

        /** Similar to BSONObj::getFieldDotted, but using FieldPath rather than a dotted string.
         *  If you pass a non-NULL positions vector, you get back a path suitable
         *  to pass to MutableDocument::setNestedField.
         *
         *  TODO a version that doesn't use FieldPath
         */
        const Value getNestedField(const FieldPath& fieldNames,
                                   vector<Position>* positions=NULL) const;

        /// Number of fields in this document. O(n)
        size_t size() const { return storage().size(); }

        /// True if this document has no fields.
        bool empty() const { return !_storage || storage().iterator().atEnd(); }

        /// Create a new FieldIterator that can be used to examine the Document's fields in order.
        FieldIterator fieldIterator() const;

        /// Convenience type for dealing with fields. Used by FieldIterator.
        typedef pair<StringData, Value> FieldPair;

        /** Get the approximate storage size of the document and sub-values in bytes.
         *  Note: Some memory may be shared with other Documents or between fields within
         *        a single Document so this can overestimate usage.
         */
        size_t getApproximateSize() const;

        /** Compare two documents.
         *
         *  BSON document field order is significant, so this just goes through
         *  the fields in order.  The comparison is done in roughly the same way
         *  as strings are compared, but comparing one field at a time instead
         *  of one character at a time.
         *
         *  @returns an integer less than zero, zero, or an integer greater than
         *           zero, depending on whether lhs < rhs, lhs == rhs, or lhs > rhs
         *  Warning: may return values other than -1, 0, or 1
         */
        static int compare(const Document& lhs, const Document& rhs);

        string toString() const;

        friend
        ostream& operator << (ostream& out, const Document& doc) { return out << doc.toString(); }

        /** Calculate a hash value.
         *
         * Meant to be used to create composite hashes suitable for
         * hashed container classes such as unordered_map.
         */
        void hash_combine(size_t &seed) const;

        /// Add this document to the BSONObj under construction with the given BSONObjBuilder.
        void toBson(BSONObjBuilder *pBsonObjBuilder) const;
        BSONObj toBson() const;

        // Support BSONObjBuilder and BSONArrayBuilder "stream" API
        friend BSONObjBuilder& operator << (BSONObjBuilderValueStream& builder, const Document& d);

        /** Return the abstract Position of a field, suitable to pass to operator[] or getField().
         *  This can potentially save time if you need to refer to a field multiple times.
         */
        Position positionOf(StringData fieldName) const { return storage().findField(fieldName); }

        /** Clone a document.
         *
         *  This should only be called by MutableDocument and tests
         *
         *  The new document shares all the fields' values with the original.
         *  This is not a deep copy.  Only the fields on the top-level document
         *  are cloned.
         */
        Document clone() const { return Document(storage().clone().get()); }

        /// members for Sorter
        struct SorterDeserializeSettings {}; // unused
        void serializeForSorter(BufBuilder& buf) const;
        static Document deserializeForSorter(BufReader& buf, const SorterDeserializeSettings&);
        int memUsageForSorter() const { return getApproximateSize(); }
        Document getOwned() const { return *this; }

        /// only for testing
        const void* getPtr() const { return _storage.get(); }

    private:
        friend class FieldIterator;
        friend class ValueStorage;
        friend class MutableDocument;
        friend class MutableValue;

        explicit Document(const DocumentStorage* ptr) : _storage(ptr) {};

        const DocumentStorage& storage() const {
            return (_storage ? *_storage : DocumentStorage::emptyDoc());
        }
        intrusive_ptr<const DocumentStorage> _storage;
    };

    inline bool operator== (const Document& l, const Document& r) {
        return Document::compare(l, r) == 0;
    }
    inline bool operator!= (const Document& l, const Document& r) {
        return Document::compare(l, r) != 0;
    }
    inline bool operator<  (const Document& l, const Document& r) {
        return Document::compare(l, r) <  0;
    }
    inline bool operator<= (const Document& l, const Document& r) {
        return Document::compare(l, r) <= 0;
    }
    inline bool operator>  (const Document& l, const Document& r) {
        return Document::compare(l, r) >  0;
    }
    inline bool operator>= (const Document& l, const Document& r) {
        return Document::compare(l, r) >= 0;
    }


    /** This class is returned by MutableDocument to allow you to modify its values.
     *  You are not allowed to hold variables of this type (enforced by the type system).
     */
    class MutableValue {
    public:
        void operator= (const Value& v) { _val = v; }

        /** These are designed to allow things like mutDoc["a"]["b"]["c"] = Value(10);
         *  It is safe to use even on nonexistent fields.
         */
        MutableValue operator[] (StringData key) { return getField(key); }
        MutableValue operator[] (Position pos)   { return getField(pos); }

        MutableValue getField(StringData key);
        MutableValue getField(Position pos);

    private:
        friend class MutableDocument;

        /// can only be constructed or copied by self and friends
        MutableValue(const MutableValue& other): _val(other._val) {}
        explicit MutableValue(Value& val): _val(val) {}

        /// Used by MutableDocument(MutableValue)
        const RefCountable*& getDocPtr() {
            if (_val.getType() != Object || _val._storage.genericRCPtr == NULL) {
                // If the current value isn't an object we replace it with a Object-typed Value.
                // Note that we can't just use Document() here because that is a NULL pointer and
                // Value doesn't refcount NULL pointers. This led to a memory leak (SERVER-10554)
                // because MutableDocument::newStorage() would set a non-NULL pointer into the Value
                // without setting the refCounter bit. While allocating a DocumentStorage here could
                // result in an allocation where none is needed, in practice this is only called
                // when we are about to add a field to the sub-document so this just changes where
                // the allocation is done.
                _val = Value(Document(new DocumentStorage()));
            }

            return _val._storage.genericRCPtr;
        }

        MutableValue& operator= (const MutableValue&); // not assignable with another MutableValue

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
    class MutableDocument : boost::noncopyable {
    public:

        /** Create a new empty Document.
         *
         *  @param expectedFields a hint at what the number of fields will be, if known.
         *         this can be used to increase memory allocation efficiency. There is
         *         no impact on correctness if this field over or under estimates.
         *
         *  TODO: find some way to convey field-name sizes to make even more efficient
         */
        MutableDocument() :_storageHolder(NULL), _storage(_storageHolder) {}
        explicit MutableDocument(size_t expectedFields);

        /// No copy yet. Copy-on-write. See storage()
        explicit MutableDocument(const Document& d) : _storageHolder(NULL)
                                                    , _storage(_storageHolder) {
            reset(d);
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
        void reset(const Document& d=Document()) { reset(d._storage.get()); }

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
        void addField(StringData fieldName, const Value& val) {
            storage().appendField(fieldName) = val;
        }

        /** Update field by key. If there is no field with that key, add one.
         *
         *  If the new value is missing(), the field is logically removed.
         */
        MutableValue operator[] (StringData key) { return getField(key); }
        void setField(StringData key, const Value& val) { getField(key) = val; }
        MutableValue getField(StringData key) {
            return MutableValue(storage().getField(key));
        }

        /// Update field by Position. Must already be a valid Position.
        MutableValue operator[] (Position pos) { return getField(pos); }
        void setField(Position pos, const Value& val) { getField(pos) = val; }
        MutableValue getField(Position pos) {
            return MutableValue(storage().getField(pos).val);
        }

        /// Logically remove a field. Note that memory usage does not decrease.
        void remove(StringData key) { getField(key) = Value(); }

        /** Gets/Sets a nested field given a path.
         *
         *  All fields along path are created as empty Documents if they don't exist
         *  or are any other type.
         */
        MutableValue getNestedField(const FieldPath& dottedField);
        void setNestedField(const FieldPath& dottedField, const Value& val) {
            getNestedField(dottedField) = val;
        }

        /// Takes positions vector from Document::getNestedField. All fields in path must exist.
        MutableValue getNestedField(const vector<Position>& positions);
        void setNestedField(const vector<Position>& positions, const Value& val) {
            getNestedField(positions) = val;
        }

        /** Convert to a read-only document and release reference.
         *
         *  Call this to indicate that you are done with this Document and will
         *  not be making further changes from this MutableDocument.
         *
         *  TODO: there are some optimizations that may make sense at freeze time.
         */
        Document freeze() {
            // This essentially moves _storage into a new Document by way of temp.
            Document ret;
            intrusive_ptr<const DocumentStorage> temp (storagePtr(), /*inc_ref_count=*/false);
            temp.swap(ret._storage);
            _storage = NULL;
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
         */
        Document peek() {
            return Document(storagePtr());
        }

    private:
        friend class MutableValue; // for access to next constructor
        explicit MutableDocument(MutableValue mv)
            : _storageHolder(NULL)
            , _storage(mv.getDocPtr())
        {}

        void reset(const DocumentStorage* ds) {
            if (_storage) intrusive_ptr_release(_storage);
            _storage = ds;
            if (_storage) intrusive_ptr_add_ref(_storage);
        }

        // This is split into 3 functions to speed up the fast-path
        DocumentStorage& storage() {
            if (MONGO_unlikely( !_storage ))
                return newStorage();

            if (MONGO_unlikely( _storage->isShared() ))
                return clonedStorage();

            // This function exists to ensure this is safe
            return const_cast<DocumentStorage&>(*storagePtr());
        }
        DocumentStorage& newStorage() {
            reset(new DocumentStorage);
            return const_cast<DocumentStorage&>(*storagePtr());
        }
        DocumentStorage& clonedStorage() {
            reset(storagePtr()->clone().get());
            return const_cast<DocumentStorage&>(*storagePtr());
        }

        // recursive helpers for same-named public methods
        MutableValue getNestedFieldHelper(const FieldPath& dottedField, size_t level);
        MutableValue getNestedFieldHelper(const vector<Position>& positions, size_t level);

        // this should only be called by storage methods and peek/freeze
        const DocumentStorage* storagePtr() const {
            dassert(!_storage || typeid(*_storage) == typeid(const DocumentStorage));
            return static_cast<const DocumentStorage*>(_storage);
        }

        // These are both const to prevent modifications bypassing storage() method.
        // They always point to NULL or an object with dynamic type DocumentStorage.
        const RefCountable* _storageHolder; // Only used in constructors and destructor
        const RefCountable*& _storage; // references either above member or genericRCPtr in a Value
    };

    /// This is the public iterator over a document
    class FieldIterator {
    public:
        explicit FieldIterator(const Document& doc)
            : _doc(doc)
            , _it(_doc.storage().iterator())
        {}

        /// Ask if there are more fields to return.
        bool more() const { return !_it.atEnd(); }

        /// Get next item and advance iterator
        Document::FieldPair next() {
            verify(more());

            Document::FieldPair fp (_it->nameSD(), _it->val);
            _it.advance();
            return fp;
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
            ValueStream(DocumentStream& builder) :builder(builder) {}

            DocumentStream& operator << (const Value& val) {
                builder._md[name] = val;
                return builder;
            }

            /// support anything directly supported by a value constructor
            template <typename T>
            DocumentStream& operator << (const T& val) {
                return *this << Value(val);
            }

            StringData name;
            DocumentStream& builder;
        };

    public:
        DocumentStream() :_stream(*this) {}

        ValueStream& operator << (const StringData& name) {
            _stream.name = name;
            return _stream;
        }

        Document done() { return _md.freeze(); }

    private:
        ValueStream _stream;
        MutableDocument _md;
    };

    class ValueArrayStream {
    public:
        ValueArrayStream& operator << (const Value& val) {
            _array.push_back(val);
            return *this;
        }

        /// support anything directly supported by a value constructor
        template <typename T>
        ValueArrayStream& operator << (const T& val) {
            return *this << Value(val);
        }

        Value done() { return Value::consume(_array); }

    private:
        vector<Value> _array;
    };

}

namespace std {
    template<>
    inline void swap(mongo::Document& lhs, mongo::Document& rhs) { lhs.swap(rhs); }
}

/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {
    inline FieldIterator Document::fieldIterator() const {
        return FieldIterator(*this);
    }

    inline MutableValue MutableValue::getField(Position pos) {
        return MutableDocument(*this).getField(pos);
    }
    inline MutableValue MutableValue::getField(StringData key) {
        return MutableDocument(*this).getField(key);
    }
}
