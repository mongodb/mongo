/* Copyright 2010 10gen Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <boost/scoped_ptr.hpp>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/cstdint.h"
#include "mongo/util/safe_num.h"

namespace mongo {
namespace mutablebson {

    class Document;
    class ElementVector;
    class Heap;
    class SiblingIterator;
    struct ElementRep;

    /*
     * The Element class represents one node in the document tree. A document is
     * identified with its root node.
     *
     * Elements are created with the help of the Document class. Once created, an
     * element can be introduced in any position in a document tree with methods
     * provided here.
     *
     * Example Usage:
     *
     *     // creation
     *     mutablebson::BasicHeap myHeap;
     *     mutablebson::Document doc(&myHeap);
     *     // Document is: {}
     *
     *     mutablebson::Element e0 = doc.makeObjElement("e0");
     *     doc.root().addChild(e0);
     *     // Document is: { e0 : {} }
     *
     *     mutablebson::Element e1 = doc.makeObjElement("e1");
     *     e0.addChild(e1);
     *     // Document is: { e0 : { e1 : {} } }
     *
     * TODO: Add iteration and search examples
     *
     */
    class Element {
    public:
        Element(Document* doc, uint32_t rep)
            : _doc(doc)
            , _rep(rep) {}

        ~Element() {}

        //
        // navigation API
        //

        Element leftChild() const;
        Element rightChild() const;
        Element leftSibling() const;
        Element rightSibling() const;
        Element parent() const;

        /** Iterate children of this node */
        SiblingIterator children();

        //
        // update API
        //

        Status addChild(Element e);
        Status addSiblingBefore(Element e);
        Status addSiblingAfter(Element e);
        Status remove();
        Status rename(const StringData& newName);
        Status move(Element newParent);

        //
        // array API
        //

        Status arraySize(uint32_t* size);
        Status peekBack(Element* ep);
        Status pushBack(Element e);
        Status popBack();
        Status peekFront(Element* ep);
        Status pushFront(Element e);
        Status popFront();
        Status get(uint32_t index, Element* ep);
        Status set(uint32_t index, Element e);

        //
        // accessors
        //

        /** Returns true if type is inlined */
        bool isInlineType() const;
 
        /** Returns the BSONTypeof this element */
        BSONType type() const;

        bool getBoolValue() const;
        int32_t getIntValue() const;
        int64_t getLongValue() const;
        OpTime getTSValue() const;
        int64_t getDateValue() const;
        double getDoubleValue() const;
        OID getOIDValue() const;

        /**
            The methods returning const char* all have the property that the returned value is
            only valid until the next non-const method is called on the underlying heap, because
            those operations might invalidate the iterators from which these values are implicitly
            derived. This happens in practice if the heap's underlying vector grows.  (andy)
        */
        const char* getStringValue() const;
        const char* getRegexValue() const;

        SafeNum getSafeNumValue() const;

        void setBoolValue(bool boolVal);
        void setIntValue(int32_t intVal);
        void setLongValue(int64_t longVal);
        void setTSValue(OpTime tsVal);
        void setDateValue(int64_t millis);
        void setDoubleValue(double doubleVal);
        void setOIDValue(const OID& oid);
        void setStringValue(const StringData& stringVal);
        void setRegexValue(const StringData& re);
        void setSafeNumValue(const SafeNum& safeNum);
        void setMinKey();
        void setMaxKey();
        void setUndefined();
        void setNull();
        void setSymbol(const StringData& symbolVal);

        /** Set the value of this element to equal the value of the
         *  provided BSONElement 'val'. The name of this Element is
         *  not modified.
         */
        void setValueFromBSONElement(const BSONElement& val);

        // additional methods needed for BSON decoding
        Status prefix(std::string* result, char delim) const;
        Status suffix(std::string* result, char delim) const;
        Status regex(std::string* result) const;
        Status regexFlags(std::string* result) const;
        Status dbrefNS(std::string* result) const;
        Status dbrefOID(std::string* result) const;
        Status codeWScopeCode(std::string* result) const;
        Status codeWScopeScope(std::string* result) const;

        bool isBoolean() const;
        bool isSimpleType() const;
        bool isNumber() const;
        bool isArray() const;
        bool isNull() const;
        bool isNonAtomic() const;

        //
        // stream API - BSONObjBuilder like API, but methods return a Status.
        //

        Status appendBool(const StringData& fieldName, bool boolVal);
        Status appendInt(const StringData& fieldName, int32_t intVal);
        Status appendLong(const StringData& fieldName, int64_t longVal);
        Status appendTS(const StringData& fieldName, OpTime tsVal);
        Status appendDate(const StringData& fieldName, int64_t millis);
        Status appendDouble(const StringData& fieldName, double doubleVal);
        Status appendOID(const StringData& fieldName, const mongo::OID& oid);
        Status appendString(const StringData& fieldName, const StringData& stringVal);
        Status appendCode(const StringData& fieldName, const StringData& code);
        Status appendSymbol(const StringData& fieldName, const StringData& symbol);
        Status appendNull(const StringData& fieldName);
        Status appendMinKey(const StringData& fieldName);
        Status appendMaxKey(const StringData& fieldName);

        Status appendDBRef(
            const StringData& fieldName, const StringData& ns, const mongo::OID& oid);
        Status appendRegex(
            const StringData& fieldName, const StringData& re, const StringData& flags);
        Status appendCodeWScope(
            const StringData& fieldName, const StringData& code, const StringData& scope);
        Status appendBinary(
            const StringData& fieldName, uint32_t len, BinDataType t, const void* bin);

        Status appendSafeNum(const StringData& fieldName, const SafeNum num);

        //
        // encapsulate state
        //

        uint32_t getRep() const { return _rep; };
        Document* getDocument() const { return _doc; }

        StringData getFieldName() const;

    private:
        inline Status checkSubtreeIsClean(Element e);

        // We carry the document in every element. The document determines the element:
        // '_rep' is resolved through the document ElementVector and Heap
        Document* _doc;
        uint32_t _rep;
    };

    /**
     * Document contains the root node, and factory methods for
     * creating new nodes.  Storage is allocated for the nodes within
     * the Content heap, and the document tree structure is stored in
     * the document ElementVector.
     *
     * For example usage, see class Element.
     */
    class Document {
        MONGO_DISALLOW_COPYING(Document);

    public:
        explicit Document(Heap* heap);
        ~Document();

        //
        // getters, (setters?)
        //

        Heap* getHeap() { return _heap; }
        const Heap* getHeap() const { return _heap; }

        //
        // The distinguished root Element of the document, which is
        // always an Object element.
        //
        Element& root() { return _root; }
        const Element& root() const { return _root; }

        //
        // factory methods
        //

        Element makeObjElement(const StringData& fieldName);
        Element makeArrayElement(const StringData& fieldName);
        Element makeNullElement(const StringData& fieldName);
        Element makeMinKeyElement(const StringData& fieldName);
        Element makeMaxKeyElement(const StringData& fieldName);
        Element makeBoolElement(const StringData& fieldName, bool boolVal);
        Element makeIntElement(const StringData& fieldName, int32_t intVal);
        Element makeLongElement(const StringData& fieldName, int64_t longVal);
        Element makeTSElement(const StringData& fieldName, OpTime tsVal);
        Element makeDateElement(const StringData& fieldName, int64_t dateVal);
        Element makeDoubleElement(const StringData& fieldName, double millis);
        Element makeOIDElement(const StringData& fieldName, const mongo::OID& oid);
        Element makeStringElement(const StringData& fieldName, const StringData& stringVal);
        Element makeCodeElement(const StringData& fieldName, const StringData& code);
        Element makeSymbolElement(const StringData& fieldName, const StringData& symbol);
        Element makeSafeNumElement(const StringData& fieldName, const SafeNum& safeNum);

        Element makeRegexElement(
            const StringData& fieldName, const StringData& regex, const StringData& flags);
        Element makeCodeWScopeElement(
            const StringData& fieldName, const StringData& code, const StringData& scope);
        Element makeDBRefElement(
            const StringData& fieldName, const StringData& ns, const mongo::OID& oid);
        Element makeBinaryElement(
            const StringData& fieldName, uint32_t len, BinDataType binType, const void* data);

    private:
        friend class Element;
        friend class SiblingIterator;

        Heap* const _heap;
        boost::scoped_ptr<ElementVector> _elements;
        Element _root;
    };

    //
    // iteration support
    //

    class Iterator {
    public:
        Iterator();

        explicit Iterator(Element e)
            : _doc(e.getDocument())
            , _theRep(e.getRep()) {}

        virtual ~Iterator() {}

        // iterator interface
        virtual Iterator& operator++() = 0;
        virtual bool done() const = 0;

        Element operator*() { return Element(getDocument(), getRep()); }

        // acessors
        Document* getDocument() const { return _doc; }
        uint32_t getRep() const { return _theRep; }

    protected:
        Document* _doc;
        uint32_t _theRep;
    };

    /** implementation: sibling iterator */
    class SiblingIterator : public Iterator {
    public:
        SiblingIterator()
            : Iterator() {}

        explicit SiblingIterator(Element e)
            : Iterator(e) {}

        virtual ~SiblingIterator() {}

        virtual SiblingIterator& operator++() {
            advance();
            return *this;
        }

        virtual bool done() const;

    private:
        bool advance();
    };

} // namespace mutablebson
} // namespace mongo
