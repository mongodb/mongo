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

#include "mongo/platform/basic.h"

#include "mongo/bson/mutable/mutable_bson.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "mongo/bson/mutable/mutable_bson_heap.h"
#include "mongo/bson/mutable/mutable_bson_internal.h"

namespace mongo {
namespace mutablebson {

namespace {
    const int32_t SHORTBIT = (1<<16);
} // namespace

    //
    // Element navigation
    //

    Element Element::leftChild() const {
        return Element(_doc, _doc->_elements->_vec[_rep]._child._left);
    }

    Element Element::rightChild() const {
        return Element(_doc, _doc->_elements->_vec[_rep]._child._right);
    }

    Element Element::leftSibling() const {
        return Element(_doc, _doc->_elements->_vec[_rep]._sibling._left);
    }

    Element Element::rightSibling() const {
        return Element(_doc, _doc->_elements->_vec[_rep]._sibling._right);
    }

    Element Element::parent() const {
        return Element(_doc, _doc->_elements->_vec[_rep]._parent);
    }

    SiblingIterator Element::children() {
        return SiblingIterator(leftChild());
    }

    StringData Element::getFieldName() const {
        return _doc->getHeap()->getStringBuffer(_doc->_elements->_vec[_rep]._nameref);
    }

    //
    // Element update API
    //

    Status Element::addChild(Element e) {

        if (isNull()) {
            return Status(ErrorCodes::IllegalOperation, "trying to add child to null node");
        }
        if (isSimpleType()) {
            return Status(ErrorCodes::IllegalOperation, "trying to add child to atomic node");
       }

        ElementRep& thisRep = _doc->_elements->_vec[_rep];
        ElementRep& newRep = e._doc->_elements->_vec[e._rep];

        /* check that new element roots a clean subtree, no dangling references */
        Status s = checkSubtreeIsClean(e);
        if (s.code() != ErrorCodes::OK) return s;

        newRep._parent = _rep;

        /* link to end of the existing sibling list */
        newRep._sibling._left = thisRep._child._right;
        if (thisRep._child._right != EMPTY_REP) {
            ElementRep& rightRep = _doc->_elements->_vec[thisRep._child._right];
            rightRep._sibling._right = e._rep;
        }
        else {
            thisRep._child._left = e._rep;
        }

        /* link as new right child */
        thisRep._child._right = e._rep;
        return Status::OK();
    }

    Status Element::addSiblingAfter(Element e) {

        if (isNull()) {
            return Status(ErrorCodes::IllegalOperation, "trying to add sibling to null node");
        }
        if (e.isNull()) {
            return Status(ErrorCodes::IllegalOperation, "trying to add null node as sibling");
        }

        ElementRep& thisRep = _doc->_elements->_vec[_rep];
        ElementRep& newRep = e._doc->_elements->_vec[e._rep];

        /* check that new element roots a clean subtree, no dangling references */
        Status s = checkSubtreeIsClean(e);
        if (s.code() != ErrorCodes::OK) return s;

        /* link in new node */
        newRep._parent = thisRep._parent;
        newRep._sibling._left = _rep;
        newRep._sibling._right = thisRep._sibling._right;
        thisRep._sibling._right = e.getRep();
        if (newRep._sibling._right != EMPTY_REP) {
            ElementRep& rightRep = _doc->_elements->_vec[newRep._sibling._right];
            rightRep._sibling._left = e.getRep();
        }

        /* fix parent right child */
        if (thisRep._parent != EMPTY_REP) {
            ElementRep& parentRep = _doc->_elements->_vec[thisRep._parent];
            if (parentRep._child._right ==_rep) {
                parentRep._child._right=e.getRep();
            }
        }

        return Status::OK();
    }

    Status Element::addSiblingBefore(Element e) {

        if (isNull()) {
            return Status(ErrorCodes::IllegalOperation, "trying to add sibling to null node");
        }
        if (e.isNull()) {
            return Status(ErrorCodes::IllegalOperation, "trying to add null node as sibling");
        }

        ElementRep& thisRep = _doc->_elements->_vec[_rep];
        ElementRep& newRep = e._doc->_elements->_vec[e._rep];

        /* check that new element roots a clean subtree, no dangling references */
        Status s = checkSubtreeIsClean(e);
        if (s.code() != ErrorCodes::OK) return s;

        /* link in new node */
        newRep._parent = thisRep._parent;
        newRep._sibling._right = _rep;
        newRep._sibling._left = thisRep._sibling._left;
        thisRep._sibling._left = e.getRep();
        if (newRep._sibling._left != EMPTY_REP) {
            ElementRep& leftRep = _doc->_elements->_vec[newRep._sibling._left];
            leftRep._sibling._right = e.getRep();
        }

        /* fix parent left child */
        if (thisRep._parent != EMPTY_REP) {
            ElementRep& parentRep = _doc->_elements->_vec[thisRep._parent];
            if (parentRep._child._left == _rep) {
                parentRep._child._left=e.getRep();
            }
        }

        return Status::OK();
    }

    Status Element::remove() {

        ElementRep& thisRep = _doc->_elements->_vec[_rep];
        if (thisRep._parent == EMPTY_REP) {
            return Status(ErrorCodes::IllegalOperation, "trying to remove document root node");
        }

        /* skip over the element being deleted */
        if (thisRep._sibling._right != EMPTY_REP) {
            ElementRep& rightRep = _doc->_elements->_vec[thisRep._sibling._right];
            rightRep._sibling._left = thisRep._sibling._left;
        }
        if (thisRep._sibling._left != EMPTY_REP) {
            ElementRep& leftRep = _doc->_elements->_vec[thisRep._sibling._left];
            leftRep._sibling._right = thisRep._sibling._right;
        }

        /* fix parent right child, if needed */
        ElementRep& parentRep = _doc->_elements->_vec[thisRep._parent];
        if (parentRep._child._right == getRep()) {
            parentRep._child._right = thisRep._sibling._left;
        }

        /* fix parent left child, if needed */
        if (parentRep._child._left == getRep()) {
            parentRep._child._left = thisRep._sibling._right;
        }

        /* clear links */
        thisRep._parent = EMPTY_REP;
        thisRep._sibling._left = EMPTY_REP;
        thisRep._sibling._right = EMPTY_REP;

        return Status::OK();
    }

    Status Element::rename(const StringData& newName) {
        ElementRep& thisRep = _doc->_elements->_vec[_rep];
        thisRep._nameref = _doc->getHeap()->putString(newName);
        return Status::OK();
    }

    Status Element::move(Element newParent) {
        Status s = remove();
        if (s.code() != ErrorCodes::OK) return s;
        s = newParent.addChild(*this);
        if (s.code() != ErrorCodes::OK) return s;
        return Status::OK();
    }


    //
    // Element array API
    //

    Status Element::arraySize(uint32_t* i) {
        if (type() != mongo::Array) {
            return Status(ErrorCodes::IllegalOperation, "size on non-array");
        }
        ElementRep& er = _doc->_elements->_vec[_rep];
        if (er._child._left == EMPTY_REP) {
            *i = 0;
        }
        else {
            Element e(_doc, er._child._left);
            uint32_t n = 0;
            for (SiblingIterator sibIt(e); !sibIt.done(); ++sibIt) ++n;
            *i = n;
        }
        return Status::OK();
    }

    Status Element::peekBack(Element* ep) {
        if (type() != mongo::Array) {
            return Status(ErrorCodes::IllegalOperation, "peekBack on non-array");
        }
        ElementRep& er = _doc->_elements->_vec[_rep];
        if (er._child._right == EMPTY_REP) {
            return Status(ErrorCodes::EmptyArrayOperation, "peekBack on empty array");
        }
        *ep = Element(_doc, er._child._right);
        return Status::OK();
    }

    Status Element::pushBack(Element e) {
        if (type() != mongo::Array) {
            return Status(ErrorCodes::IllegalOperation, "pushBack on non-array");
        }
        addChild(e);
        return Status::OK();
    }

    Status Element::popBack() {
        if (type() != mongo::Array) {
            return Status(ErrorCodes::IllegalOperation, "popBack on non-array");
        }
        ElementRep& er = _doc->_elements->_vec[_rep];
        if (er._child._right == EMPTY_REP) {
            return Status(ErrorCodes::EmptyArrayOperation, "popBack on empty array");
        }
        rightChild().remove();
        return Status::OK();
    }

    Status Element::peekFront(Element* ep) {
        if (type() != mongo::Array) {
            return Status(ErrorCodes::IllegalOperation, "peekFront on non-array");
        }
        ElementRep& er = _doc->_elements->_vec[_rep];
        if (er._child._left == EMPTY_REP) {
            return Status(ErrorCodes::EmptyArrayOperation, "peekFront on empty array");
        }
        *ep = Element(_doc, er._child._left);
        return Status::OK();
    }

    Status Element::pushFront(Element e) {
        if (type() != mongo::Array) {
            return Status(ErrorCodes::IllegalOperation, "pushFront on non-array");
        }
        ElementRep& er = _doc->_elements->_vec[_rep];
        if (er._child._left == EMPTY_REP) {
            addChild(e);
        }
        else {
            leftChild().addSiblingBefore(e);
        }
        return Status::OK();
    }

    Status Element::popFront() {
        if (type() != mongo::Array) {
            return Status(ErrorCodes::IllegalOperation, "popFront on non-array");
        }
        ElementRep& er = _doc->_elements->_vec[_rep];
        if (er._child._left == EMPTY_REP) {
            return Status(ErrorCodes::EmptyArrayOperation, "popFront on empty array");
        }
        leftChild().remove();
        return Status::OK();
    }

    Status Element::get(uint32_t index, Element* ep) {
        if (type() != mongo::Array) {
            return Status(ErrorCodes::IllegalOperation, "get(index, &e) on non-array");
        }
        ElementRep& thisRep = _doc->_elements->_vec[_rep];
        if (thisRep._child._left == EMPTY_REP) {
            return Status(ErrorCodes::IllegalOperation, "get(index, &e) on empty array");
        }
        Element eLeft(_doc, thisRep._child._left);
        uint32_t n = 0;
        SiblingIterator sibIt(eLeft);
        for (; !sibIt.done() && n < index; ++sibIt) ++n;
        if (sibIt.done()) {
            *ep = Element(_doc, EMPTY_REP);
            return Status(ErrorCodes::IllegalOperation, "get(index, &e) out of bounds");
        }
        *ep = (*sibIt);
        return Status::OK();
    }

    Status Element::set(uint32_t index, Element e) {
        if (type() != mongo::Array) {
            return Status(ErrorCodes::IllegalOperation, "set(index, e) on non-array");
        }
        if (!e.isSimpleType()) {
            return Status(ErrorCodes::IllegalOperation,
                              "set(index, e) source is non-simple type node");
        }
        ElementRep& thisRep = _doc->_elements->_vec[_rep];
        if (thisRep._child._left == EMPTY_REP) {
            return Status(ErrorCodes::IllegalOperation, "set(index, e) on empty array");
        }
        Element eLeft(_doc, thisRep._child._left);
        if (!eLeft.isSimpleType()) {
            return Status(ErrorCodes::IllegalOperation,
                              "set(index, e) target is non-simple type node");
        }
        uint32_t n = 0;
        SiblingIterator sibIt(eLeft);
        for (; !sibIt.done() && n < index; ++sibIt) ++n;
        if (sibIt.done()) {
            return Status(ErrorCodes::IllegalOperation, "get(index, e) out of bounds");
        }
ElementRep& dstRep = _doc->_elements->_vec[(*sibIt)._rep];
        ElementRep& srcRep = _doc->_elements->_vec[e._rep];
        dstRep._value = srcRep._value;
        return Status::OK();
    }

    //
    // Element get/set value interface
    //

    bool Element::getBoolValue() const {
        return _doc->_elements->_vec[_rep]._value.boolVal;
    }
    int32_t Element::getIntValue() const {
        return _doc->_elements->_vec[_rep]._value.intVal;
    }
    int64_t Element::getLongValue() const {
        return _doc->_elements->_vec[_rep]._value.longVal;
    }
    OpTime Element::getTSValue() const {
        return OpTime(_doc->_elements->_vec[_rep]._value.tsVal);
    }
    int64_t Element::getDateValue() const {
        return _doc->_elements->_vec[_rep]._value.dateVal;
    }
    double Element::getDoubleValue() const {
        return _doc->_elements->_vec[_rep]._value.doubleVal;
    }
    OID Element::getOIDValue() const {
        return OID(
            reinterpret_cast<const unsigned char (&)[OID::kOIDSize]>(
                _doc->_elements->_vec[_rep]._value.shortStr));
    }
    const char* Element::getRegexValue() const {
        return getStringValue();
    }

    const char* Element::getStringValue() const {
        if (isInlineType()) {
            return &_doc->_elements->_vec[_rep]._value.shortStr[0];
        }
        else {
            return _doc->getHeap()->getStringBuffer(_doc->_elements->_vec[_rep]._value.valueRef);
        }
    }

    SafeNum Element::getSafeNumValue() const {
        switch (_doc->_elements->_vec[_rep]._type) {
        case mongo::NumberInt:
            return SafeNum(_doc->_elements->_vec[_rep]._value.intVal);
        case mongo::NumberLong:
            return SafeNum(static_cast<long long int>(_doc->_elements->_vec[_rep]._value.longVal));
        case mongo::NumberDouble:
            return SafeNum(_doc->_elements->_vec[_rep]._value.doubleVal);
        default:
            return SafeNum();
        }
    }


    void Element::setBoolValue(bool boolVal) {
        ElementRep& e = _doc->_elements->_vec[_rep];
        e._type = mongo::Bool;
        e._value.boolVal = boolVal;
    }

    void Element::setIntValue(int32_t intVal) {
        ElementRep& e = _doc->_elements->_vec[_rep];
        e._type = mongo::NumberInt;
        e._value.intVal = intVal;
    }

    void Element::setLongValue(int64_t longVal) {
        ElementRep& e = _doc->_elements->_vec[_rep];
        e._type = mongo::NumberLong;
        e._value.longVal = longVal;
    }

    void Element::setTSValue(OpTime tsVal) {
        ElementRep& e = _doc->_elements->_vec[_rep];
        e._type = mongo::Timestamp;
        e._value.tsVal = tsVal.asDate();
    }

    void Element::setDateValue(int64_t dateVal) {
        ElementRep& e = _doc->_elements->_vec[_rep];
        e._type = mongo::Date;
        e._value.dateVal = dateVal;
    }

    void Element::setDoubleValue(double doubleVal) {
        ElementRep& e = _doc->_elements->_vec[_rep];
        e._type = mongo::NumberDouble;
        e._value.doubleVal = doubleVal;
    }

    void Element::setOIDValue(const OID& oid) {
        ElementRep& e = _doc->_elements->_vec[_rep];
        e._type = mongo::jstOID;
        BOOST_STATIC_ASSERT(mongo::OID::kOIDSize <= sizeof(e._value.shortStr));
        std::memcpy(e._value.shortStr, reinterpret_cast<const char*>(oid.getData()), OID::kOIDSize);
    }

    void Element::setRegexValue(const StringData& re) {
        // type is set to "string" in setStringValue
        setStringValue(re);
        _doc->_elements->_vec[_rep]._type = mongo::RegEx;
    }

    void Element::setStringValue(const StringData& stringVal) {
        ElementRep& e = _doc->_elements->_vec[_rep];
        e._type = mongo::String;
        if (stringVal.size() < SHORT_LIMIT) {
            stringVal.copyTo( e._value.shortStr, true );
            e._type |= SHORTBIT;
        }
        else {
            e._value.valueRef = _doc->_heap->putString(stringVal);
        }
    }

    void Element::setSafeNumValue(const SafeNum& safeNumVal) {
        ElementRep& e = _doc->_elements->_vec[_rep];
        e._type = safeNumVal.type();
        switch (e._type) {
        case mongo::NumberInt:
            e._value.intVal = safeNumVal._value.int32Val;
            break;
        case mongo::NumberLong:
            e._value.longVal = safeNumVal._value.int64Val;
            break;
        case mongo::NumberDouble:
            e._value.doubleVal = safeNumVal._value.doubleVal;
            break;
        default:
            // Invalid type - e._type was set to EOO above, so we're done
            break;
        }
    }

    void Element::setMinKey() {
        ElementRep& e = _doc->_elements->_vec[_rep];
        e._type = mongo::MinKey;
    }

    void Element::setMaxKey() {
        ElementRep& e = _doc->_elements->_vec[_rep];
        e._type = mongo::MinKey;
    }

    void Element::setUndefined() {
        ElementRep& e = _doc->_elements->_vec[_rep];
        e._type = mongo::Undefined;
    }

    void Element::setNull() {
        ElementRep& e = _doc->_elements->_vec[_rep];
        e._type = mongo::jstNULL;
    }

    void Element::setSymbol(const StringData& symbolVal) {
        ElementRep& e = _doc->_elements->_vec[_rep];
        e._type = mongo::Symbol;
        if (symbolVal.size() < SHORT_LIMIT) {
            symbolVal.copyTo( e._value.shortStr, true );
            e._type |= SHORTBIT;
        }
        else {
            e._value.valueRef = _doc->_heap->putString(symbolVal);
        }
    }

    void Element::setValueFromBSONElement(const BSONElement& val) {
        switch(val.type()) {
        case MinKey:
            setMinKey();
            break;
        case EOO:
            verify(false);
            break;
        case NumberDouble:
            setDoubleValue(val._numberDouble());
            break;
        case String:
            setStringValue(StringData(val.valuestr(), val.valuestrsize()));
            break;
        case Object:
            verify(false);
            break;
        case Array:
            verify(false);
            break;
        case BinData:
            verify(false);
            break;
        case Undefined:
            setUndefined();
            break;
        case jstOID:
            setOIDValue(val.__oid());
            break;
        case Bool:
            setBoolValue(val.boolean());
            break;
        case Date:
            setDateValue(val.date());
            break;
        case jstNULL:
            setNull();
            break;
        case RegEx:
            verify(false);
            break;
        case DBRef:
            verify(false);
            break;
        case Code:
            verify(false);
            break;
        case Symbol:
            setSymbol(StringData(val.valuestr(), val.valuestrsize()));
            break;
        case CodeWScope:
            verify(false);
            break;
        case NumberInt:
            setIntValue(val._numberInt());
            break;
        case Timestamp:
            setTSValue(val._opTime());
            break;
        case NumberLong:
            setLongValue(val._numberLong());
            break;
        case MaxKey:
            setMaxKey();
            break;
        default:
            verify(false);
            break;
        }
    }

    //
    // decoders
    //

    Status Element::prefix(std::string* result, char delim) const {
        std::string s = getStringValue();
        size_t n = s.find(delim);
        if (n == std::string::npos) {
            return Status(ErrorCodes::IllegalOperation, "expecting regex format /PAT/flags");
        }
        size_t m = s.find_last_of(delim);
        if (m == std::string::npos || m == n) {
            return Status(ErrorCodes::IllegalOperation, "expecting regex format /pat/FLAGS");
        }
        result->append(s.substr(n+1,m-n-1));
        return Status::OK();
    }

    Status Element::suffix(std::string* result, char delim) const {
        std::string s = getStringValue();
        size_t n = s.find(delim);
        if (n == std::string::npos) {
            return Status(ErrorCodes::IllegalOperation, "expecting regex format ./.pat/flags");
        }
        size_t m = s.find_last_of(delim);
        if (m == std::string::npos || m == n) {
            return Status(ErrorCodes::IllegalOperation, "expecting regex format /pat./.flags");
        }
        result->append(s.substr(m+1));
        return Status::OK();
    }

    Status Element::regex(std::string* result) const {
        return prefix(result, '/');
    }

    Status Element::regexFlags(std::string* result) const {
        return suffix(result, '/');
    }

    Status Element::dbrefNS(std::string* result) const {
        return prefix(result, ':');
    }

    Status Element::dbrefOID(std::string* result) const {
        return suffix(result, ':');
    }

    Status Element::codeWScopeCode(std::string* result) const {
        return prefix(result, '|');
    }

    Status Element::codeWScopeScope(std::string* result) const {
        return suffix(result, '|');
    }

    //
    // Element predicates
    //

    bool Element::isBoolean() const {
        return (_doc->_elements->_vec[_rep]._type == mongo::Bool);
    }

    bool Element::isNonAtomic() const {
        return !isSimpleType();
    }

    bool Element::isArray() const {
        return (type() == mongo::Array);
    }

    bool Element::isSimpleType() const {
        switch(type()) {
        case mongo::NumberLong:
        case mongo::NumberDouble:
        case mongo::NumberInt:
        case mongo::String:
        case mongo::Bool:
        case mongo::Date:
        case jstOID:
            return true;
        default:
            return false;
        }
    }

    bool Element::isNumber() const {
        BSONType t = type();
        if (t == mongo::NumberDouble ||
            t == mongo::NumberInt ||
            t == mongo::NumberLong) return true;
        return false;
    }

    bool Element::isNull() const {
        return (type() == mongo::jstNULL);
    }

    BSONType Element::type() const {
        ElementRep& e = _doc->_elements->_vec[_rep];
        int32_t t = e._type;
        if ((t >= 0) && (t & SHORTBIT)) t ^= SHORTBIT;
        return static_cast<BSONType>(t);
    }

    bool Element::isInlineType() const {
        ElementRep& e = _doc->_elements->_vec[_rep];
        int32_t t = e._type;
        if ((t >= 0) && (t & SHORTBIT)) return true;
        return false;
    }

    // TODO: These should be probably be made inline.
    Status Element::appendBool(const StringData& fieldName, bool boolVal) {
        return addChild(_doc->makeBoolElement(fieldName, boolVal));
    }

    Status Element::appendInt(const StringData& fieldName, int32_t intVal) {
        return addChild(_doc->makeIntElement(fieldName, intVal));
    }

    Status Element::appendLong(const StringData& fieldName, int64_t longVal) {
        return addChild(_doc->makeLongElement(fieldName, longVal));
    }

    Status Element::appendTS(const StringData& fieldName, OpTime tsVal) {
        return addChild(_doc->makeTSElement(fieldName, tsVal));
    }

    Status Element::appendDate(const StringData& fieldName, int64_t millis) {
        return addChild(_doc->makeDateElement(fieldName, millis));
    }

    Status Element::appendDouble(const StringData& fieldName, double doubleVal) {
        return addChild(_doc->makeDoubleElement(fieldName, doubleVal));
    }

    Status Element::appendOID(const StringData& fieldName, const mongo::OID& oid) {
        return addChild(_doc->makeOIDElement(fieldName, oid));
    }

    Status Element::appendString(const StringData& fieldName, const StringData& stringVal) {
        return addChild(_doc->makeStringElement(fieldName, stringVal));
    }

    Status Element::appendCode(const StringData& fieldName, const StringData& code) {
        return addChild(_doc->makeCodeElement(fieldName, code));
    }

    Status Element::appendSymbol(const StringData& fieldName, const StringData& symbol) {
        return addChild(_doc->makeSymbolElement(fieldName, symbol));
    }

    Status Element::appendNull(const StringData& fieldName) {
        return addChild(_doc->makeNullElement(fieldName));
    }

    Status Element::appendMinKey(const StringData& fieldName) {
        return addChild(_doc->makeMinKeyElement(fieldName));
    }

    Status Element::appendMaxKey(const StringData& fieldName) {
        return addChild(_doc->makeMaxKeyElement(fieldName));
    }

    Status Element::appendRegex( const StringData& fieldName,
        const StringData& re, const StringData& flags) {
        return addChild(_doc->makeRegexElement(fieldName, re, flags));
    }

    Status Element::appendCodeWScope( const StringData& fieldName,
        const StringData& code, const StringData& scope) {
        return addChild(_doc->makeCodeWScopeElement(fieldName, code, scope));
    }

    Status Element::appendDBRef( const StringData& fieldName,
        const StringData& ns, const mongo::OID& oid) {
        return addChild(_doc->makeDBRefElement(fieldName, ns, oid));
    }

    Status Element::appendBinary( const StringData& fieldName,
        uint32_t len, mongo::BinDataType binType, const void* data) {
        return addChild(_doc->makeBinaryElement(fieldName, len, binType, data));
    }

    Status Element::appendSafeNum(const StringData& fieldName, const SafeNum num) {
        return addChild(_doc->makeSafeNumElement(fieldName, num));
    }

    //
    // Element helper methods
    //

    /** check that new element roots a clean subtree, no dangling references */
    inline Status Element::checkSubtreeIsClean(Element e) {
        ElementRep& rep = e._doc->_elements->_vec[e._rep];
        if (rep._sibling._left != EMPTY_REP) {
            return Status(ErrorCodes::IllegalOperation, "addChild: dangling left sibling");
        }
        if (rep._sibling._right != EMPTY_REP) {
            return Status(ErrorCodes::IllegalOperation, "addChild: dangling right sibling");
        }
        if (rep._parent != EMPTY_REP) {
            return Status(ErrorCodes::IllegalOperation, "addChild: dangling parent");
        }
        return Status::OK();
    }


    //
    // Document Implementation
    //

    Document::Document(Heap* heap) :
        _heap(heap),
        _elements(new ElementVector),
        _root(makeObjElement(StringData("", StringData::LiteralTag()))) {
    }

    Document::~Document() {}

    // Document factory methods

    Element Document::makeObjElement(const StringData& fieldName) {
        uint32_t rep = _elements->size();
        uint32_t nameref = _heap->putString(fieldName);
        _elements->push_back(ElementRep(mongo::Object, nameref));
        return Element(this, rep);
    }

    Element Document::makeArrayElement(const StringData& fieldName) {
        uint32_t rep = _elements->size();
        uint32_t nameref = _heap->putString(fieldName);
        _elements->push_back(ElementRep(mongo::Array, nameref));
        return Element(this, rep);
    }

    Element Document::makeNullElement(const StringData& fieldName) {
        uint32_t rep = _elements->size();
        uint32_t nameref = _heap->putString(fieldName);
        _elements->push_back(ElementRep(mongo::jstNULL, nameref));
        return Element(this, rep);
    }

    Element Document::makeMinKeyElement(const StringData& fieldName) {
        uint32_t rep = _elements->size();
        uint32_t nameref = _heap->putString(fieldName);
        _elements->push_back(ElementRep(mongo::MinKey, nameref));
        return Element(this, rep);
    }

    Element Document::makeMaxKeyElement(const StringData& fieldName) {
        uint32_t rep = _elements->size();
        uint32_t nameref = _heap->putString(fieldName);
        _elements->push_back(ElementRep(mongo::MaxKey, nameref));
        return Element(this, rep);
    }

    Element Document::makeBoolElement(const StringData& fieldName, bool b) {
        uint32_t rep = _elements->size();
        uint32_t nameref = _heap->putString(fieldName);
        ValueType val;
        val.boolVal = b;
        _elements->push_back(ElementRep(mongo::Bool, nameref, val));
        return Element(this, rep);
    }

    Element Document::makeIntElement(const StringData& fieldName, int32_t i) {
        uint32_t rep = _elements->size();
        uint32_t nameref = _heap->putString(fieldName);
        ValueType val;
        val.intVal = i;
        _elements->push_back(ElementRep(mongo::NumberInt, nameref, val));
        return Element(this, rep);
    }

    Element Document::makeLongElement(const StringData& fieldName, int64_t j) {
        uint32_t rep = _elements->size();
        uint32_t nameref = _heap->putString(fieldName);
        ValueType val;
        val.longVal = j;
        _elements->push_back(ElementRep(mongo::NumberLong, nameref, val));
        return Element(this, rep);
    }

    Element Document::makeTSElement(const StringData& fieldName, OpTime ts) {
        uint32_t rep = _elements->size();
        uint32_t nameref = _heap->putString(fieldName);
        ValueType val;
        val.tsVal = ts.asDate();
        _elements->push_back(ElementRep(mongo::Timestamp, nameref, val));
        return Element(this, rep);
    }

    Element Document::makeDateElement(const StringData& fieldName, int64_t date) {
        uint32_t rep = _elements->size();
        uint32_t nameref = _heap->putString(fieldName);
        ValueType val;
        val.dateVal = date;
        _elements->push_back(ElementRep(mongo::Date, nameref, val));
        return Element(this, rep);
    }

    Element Document::makeDoubleElement(const StringData& fieldName, double d) {
        uint32_t rep = _elements->size();
        uint32_t nameref = _heap->putString(fieldName);
        ValueType val;
        val.doubleVal = d;
        _elements->push_back(ElementRep(mongo::NumberDouble, nameref, val));
        return Element(this, rep);
    }

    Element Document::makeOIDElement(const StringData& fieldName, const mongo::OID& oid) {
        uint32_t rep = _elements->size();
        uint32_t nameref = _heap->putString(fieldName);
        ValueType val;
        memcpy(val.shortStr, (char*)oid.getData(), 12);
        _elements->push_back(ElementRep(mongo::jstOID, nameref, val));
        return Element(this, rep);
    }

    Element Document::makeStringElement(const StringData& fieldName, const StringData& stringVal) {
        uint32_t rep = _elements->size();
        uint32_t nameref = _heap->putString(fieldName);
        ValueType val;
        if (stringVal.size() < SHORT_LIMIT) {
            stringVal.copyTo( val.shortStr, true );
            _elements->push_back(ElementRep(mongo::String|SHORTBIT, nameref, val));
        }
        else {
            val.valueRef = _heap->putString(stringVal);
            _elements->push_back(ElementRep(mongo::String, nameref, val));
        }
        return Element(this, rep);
    }

    Element Document::makeRegexElement(
        const StringData& fieldName, const StringData& re, const StringData& flags) {
        uint32_t rep = _elements->size();
        uint32_t nameref = _heap->putString(fieldName);
        ValueType val;
        val.valueRef = _heap->putString(re);
        _elements->push_back(ElementRep(mongo::String, nameref, val));
        return Element(this, rep);
    }

    Element Document::makeCodeElement(
        const StringData& fieldName, const StringData& code) {
        uint32_t rep = _elements->size();
        uint32_t nameref = _heap->putString(fieldName);
        ValueType val;
        val.valueRef = _heap->putString(code);
        _elements->push_back(ElementRep(mongo::Code, nameref, val));
        return Element(this, rep);
    }

    Element Document::makeSymbolElement( const StringData& fieldName, const StringData& symbol) {
        uint32_t rep = _elements->size();
        uint32_t nameref = _heap->putString(fieldName);
        ValueType val;
        val.valueRef = _heap->putString(symbol);
        _elements->push_back(ElementRep(mongo::Symbol, nameref, val));
        return Element(this, rep);
    }

    /** Question: Are these three cases sub-objects or primitive types?
    Existing code appears to form strange concatenated strings with embedded nulls.
    Stubbed for now, pending better understanding. */

    Element Document::makeCodeWScopeElement( const StringData& fieldName,
        const StringData& theCode, const StringData& theScope) {
        return Element(this,EMPTY_REP);
    }

    Element Document::makeDBRefElement( const StringData& fieldName,
        const StringData& ns, const mongo::OID& oid) {
        return Element(this,EMPTY_REP);
    }

    Element Document::makeBinaryElement( const StringData& fieldName,
        uint32_t len, mongo::BinDataType binType, const void* data) {
        return Element(this,EMPTY_REP);
    }

    Element Document::makeSafeNumElement(const StringData& fieldName, const SafeNum& safeNum) {
        uint32_t rep = _elements->size();
        uint32_t nameref = _heap->putString(fieldName);
        ValueType val;

        switch (safeNum.type()) {
        case mongo::NumberInt:
            val.intVal = safeNum._value.int32Val;
            break;
        case mongo::NumberLong:
            val.longVal = safeNum._value.int64Val;
            break;
        case mongo::NumberDouble:
            val.doubleVal = safeNum._value.doubleVal;
            break;
        default:
            // Invalid SafeNum - type is set to EOO below
            break;
        }

        _elements->push_back(ElementRep(safeNum.type(), nameref, val));
        return Element(this, rep);
    }

    //
    // Iterator base class
    //

    Iterator::Iterator() : _doc(NULL), _theRep(EMPTY_REP) {}

    //
    // SiblingIterator
    //

    bool SiblingIterator::done() const { return (_theRep == EMPTY_REP); }

    bool SiblingIterator::advance() {
        if (_theRep == EMPTY_REP) return false;
        ElementRep& e = _doc->_elements->_vec[_theRep];
        _theRep = e._sibling._right;
        return (_theRep == EMPTY_REP);
    }

} // namespace mutablebson
} // namespace mongo
