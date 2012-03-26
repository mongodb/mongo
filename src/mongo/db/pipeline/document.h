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
 */

#pragma once

#include "pch.h"

#include "util/intrusive_counter.h"

namespace mongo {
    class BSONObj;
    class DependencyTracker;
    class FieldIterator;
    class Value;

    class Document :
        public IntrusiveCounterUnsigned {
    public:
        ~Document();

        /*
          Create a new Document from the given BSONObj.

          Document field values may be pointed to in the BSONObj, so it
          must live at least as long as the resulting Document.

          LATER - use an abstract class for the dependencies; something like
          a "lookup(const string &fieldName)" so there can be other
          implementations.

          @returns shared pointer to the newly created Document
        */
        static intrusive_ptr<Document> createFromBsonObj(
            BSONObj *pBsonObj, const DependencyTracker *pDependencies = NULL);

        /*
          Create a new empty Document.

          @param sizeHint a hint at what the number of fields will be; if
            known, this can be used to increase memory allocation efficiency
          @returns shared pointer to the newly created Document
        */
        static intrusive_ptr<Document> create(size_t sizeHint = 0);

        /*
          Clone a document.

          The new document shares all the fields' values with the original.

          This is not a deep copy.  Only the fields on the top-level document
          are cloned.

          @returns the shallow clone of the document
        */
        intrusive_ptr<Document> clone();

        /*
          Add this document to the BSONObj under construction with the
          given BSONObjBuilder.
        */
        void toBson(BSONObjBuilder *pBsonObjBuilder);

        /*
          Create a new FieldIterator that can be used to examine the
          Document's fields.
        */
        FieldIterator *createFieldIterator();

        /*
          Get the value of the specified field.

          @param fieldName the name of the field
          @return point to the requested field
        */
        intrusive_ptr<const Value> getValue(const string &fieldName);

        /*
          Add the given field to the Document.

          BSON documents' fields are ordered; the new Field will be
          appened to the current list of fields.

          It is an error to add a field that has the same name as another
          field.
        */
        void addField(const string &fieldName,
                      const intrusive_ptr<const Value> &pValue);

        /*
          Set the given field to be at the specified position in the
          Document.  This will replace any field that is currently in that
          position.  The index must be within the current range of field
          indices.

          pValue.get() may be NULL, in which case the field will be
          removed.  fieldName is ignored in this case.

          @param index the field index in the list of fields
          @param fieldName the new field name
          @param pValue the new Value
        */
        void setField(size_t index,
                      const string &fieldName,
                      const intrusive_ptr<const Value> &pValue);

        /*
          Convenience type for dealing with fields.
         */
        typedef pair<string, intrusive_ptr<const Value> > FieldPair;

        /*
          Get the indicated field.

          @param index the field index in the list of fields
          @returns the field name and value of the field
         */
        FieldPair getField(size_t index) const;

        /*
          Get the number of fields in the Document.

          @returns the number of fields in the Document
         */
        size_t getFieldCount() const;

        /*
          Get the index of the given field.

          @param fieldName the name of the field
          @returns the index of the field, or if it does not exist, the number
            of fields (getFieldCount())
        */
        size_t getFieldIndex(const string &fieldName) const;

        /*
          Get a field by name.

          @param fieldName the name of the field
          @returns the value of the field
        */
        intrusive_ptr<const Value> getField(const string &fieldName) const;

        /*
          Get the approximate storage size of the document, in bytes.

          Under the assumption that field name strings are shared, they are
          not included in the total.

          @returns the approximate storage
        */
        size_t getApproximateSize() const;

        /*
          Compare two documents.

          BSON document field order is significant, so this just goes through
          the fields in order.  The comparison is done in roughly the same way
          as strings are compared, but comparing one field at a time instead
          of one character at a time.
        */
        static int compare(const intrusive_ptr<Document> &rL,
                           const intrusive_ptr<Document> &rR);

        static string idName; // shared "_id"

        /*
          Calculate a hash value.

          Meant to be used to create composite hashes suitable for
          boost classes such as unordered_map<>.

          @param seed value to augment with this' hash
        */
        void hash_combine(size_t &seed) const;

    private:
        friend class FieldIterator;

        Document(size_t sizeHint);
        Document(BSONObj *pBsonObj, const DependencyTracker *pDependencies);

        /* these two vectors parallel each other */
        vector<string> vFieldName;
        vector<intrusive_ptr<const Value> > vpValue;
    };


    class FieldIterator :
            boost::noncopyable {
    public:
        /*
          Ask if there are more fields to return.

          @return true if there are more fields, false otherwise
        */
        bool more() const;

        /*
          Move the iterator to point to the next field and return it.

          @return the next field's <name, Value>
        */
        Document::FieldPair next();

    private:
        friend class Document;

        /*
          Constructor.

          @param pDocument points to the document whose fields are being
              iterated
        */
        FieldIterator(const intrusive_ptr<Document> &pDocument);

        /*
          We'll hang on to the original document to ensure we keep the
          fieldPtr vector alive.
        */
        intrusive_ptr<Document> pDocument;
        size_t index; // current field in iteration
    };
}


/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {

    inline size_t Document::getFieldCount() const {
        return vFieldName.size();
    }
    
    inline Document::FieldPair Document::getField(size_t index) const {
        verify( index < vFieldName.size() );
        return FieldPair(vFieldName[index], vpValue[index]);
    }

}
