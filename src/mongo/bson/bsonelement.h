// bsonelement.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include <string.h> // strlen
#include <string>
#include <vector>

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/platform/cstdint.h"
#include "mongo/platform/float_utils.h"

namespace mongo {
    class OpTime;
    class BSONObj;
    class BSONElement;
    class BSONObjBuilder;
}

namespace bson {
    typedef mongo::BSONElement be;
    typedef mongo::BSONObj bo;
    typedef mongo::BSONObjBuilder bob;
}

namespace mongo {

    /* l and r MUST have same type when called: check that first. */
    int compareElementValues(const BSONElement& l, const BSONElement& r);


    /** BSONElement represents an "element" in a BSONObj.  So for the object { a : 3, b : "abc" },
        'a : 3' is the first element (key+value).

        The BSONElement object points into the BSONObj's data.  Thus the BSONObj must stay in scope
        for the life of the BSONElement.

        internals:
        <type><fieldName    ><value>
        -------- size() ------------
        -fieldNameSize-
        value()
        type()
    */
    class BSONElement {
    public:
        /** These functions, which start with a capital letter, throw a MsgAssertionException if the
            element is not of the required type. Example:

            std::string foo = obj["foo"].String(); // std::exception if not a std::string type or DNE
        */
        std::string String()        const { return chk(mongo::String).str(); }
        Date_t Date()               const { return chk(mongo::Date).date(); }
        double Number()             const { return chk(isNumber()).number(); }
        double Double()             const { return chk(NumberDouble)._numberDouble(); }
        long long Long()            const { return chk(NumberLong)._numberLong(); }
        int Int()                   const { return chk(NumberInt)._numberInt(); }
        bool Bool()                 const { return chk(mongo::Bool).boolean(); }
        std::vector<BSONElement> Array() const; // see implementation for detailed comments
        mongo::OID OID()            const { return chk(jstOID).__oid(); }
        void Null()                 const { chk(isNull()); } // throw MsgAssertionException if not null
        void OK()                   const { chk(ok()); }     // throw MsgAssertionException if element DNE

        /** @return the embedded object associated with this field.
            Note the returned object is a reference to within the parent bson object. If that 
            object is out of scope, this pointer will no longer be valid. Call getOwned() on the 
            returned BSONObj if you need your own copy.
            throws UserException if the element is not of type object.
        */
        BSONObj Obj()               const;

        /** populate v with the value of the element.  If type does not match, throw exception.
            useful in templates -- see also BSONObj::Vals().
        */
        void Val(Date_t& v)         const { v = Date(); }
        void Val(long long& v)      const { v = Long(); }
        void Val(bool& v)           const { v = Bool(); }
        void Val(BSONObj& v)        const;
        void Val(mongo::OID& v)     const { v = OID(); }
        void Val(int& v)            const { v = Int(); }
        void Val(double& v)         const { v = Double(); }
        void Val(std::string& v)    const { v = String(); }

        /** Use ok() to check if a value is assigned:
            if( myObj["foo"].ok() ) ...
        */
        bool ok() const { return !eoo(); }

        std::string toString( bool includeFieldName = true, bool full=false) const;
        void toString(StringBuilder& s, bool includeFieldName = true, bool full=false, int depth=0) const;
        std::string jsonString( JsonStringFormat format, bool includeFieldNames = true, int pretty = 0 ) const;
        operator std::string() const { return toString(); }

        /** Returns the type of the element */
        BSONType type() const { return (BSONType) *reinterpret_cast< const signed char * >(data); }

        /** retrieve a field within this element
            throws exception if *this is not an embedded object
        */
        BSONElement operator[] (const std::string& field) const;

        /** See canonicalizeBSONType in bsontypes.h */
        int canonicalType() const { return canonicalizeBSONType(type()); }

        /** Indicates if it is the end-of-object element, which is present at the end of
            every BSON object.
        */
        bool eoo() const { return type() == EOO; }

        /** Size of the element.
            @param maxLen If maxLen is specified, don't scan more than maxLen bytes to calculate size.
        */
        int size( int maxLen ) const;
        int size() const;

        /** Wrap this element up as a singleton object. */
        BSONObj wrap() const;

        /** Wrap this element up as a singleton object with a new name. */
        BSONObj wrap( const StringData& newName) const;

        /** field name of the element.  e.g., for
            name : "Joe"
            "name" is the fieldname
        */
        const char * fieldName() const {
            if ( eoo() ) return ""; // no fieldname for it.
            return data + 1;
        }

        /**
         * NOTE: size includes the NULL terminator.
         */
        int fieldNameSize() const {
            if ( fieldNameSize_ == -1 )
                fieldNameSize_ = (int)strlen( fieldName() ) + 1;
            return fieldNameSize_;
        }

        const StringData fieldNameStringData() const {
            return StringData(fieldName(), fieldNameSize() - 1);
        }

        /** raw data of the element's value (so be careful). */
        const char * value() const {
            return (data + fieldNameSize() + 1);
        }
        /** size in bytes of the element's value (when applicable). */
        int valuesize() const {
            return size() - fieldNameSize() - 1;
        }

        bool isBoolean() const { return type() == mongo::Bool; }

        /** @return value of a boolean element.
            You must assure element is a boolean before
            calling. */
        bool boolean() const {
            return *value() ? true : false;
        }

        bool booleanSafe() const { return isBoolean() && boolean(); }

        /** Retrieve a java style date value from the element.
            Ensure element is of type Date before calling.
            @see Bool(), trueValue()
        */
        Date_t date() const {
            return *reinterpret_cast< const Date_t* >( value() );
        }

        /** Convert the value to boolean, regardless of its type, in a javascript-like fashion
            (i.e., treats zero and null and eoo as false).
        */
        bool trueValue() const;

        /** True if number, string, bool, date, OID */
        bool isSimpleType() const;

        /** True if element is of a numeric type. */
        bool isNumber() const;

        /** Return double value for this field. MUST be NumberDouble type. */
        double _numberDouble() const {return (reinterpret_cast< const PackedDouble* >( value() ))->d; }
        /** Return int value for this field. MUST be NumberInt type. */
        int _numberInt() const {return *reinterpret_cast< const int* >( value() ); }
        /** Return long long value for this field. MUST be NumberLong type. */
        long long _numberLong() const {return *reinterpret_cast< const long long* >( value() ); }

        /** Retrieve int value for the element safely.  Zero returned if not a number. */
        int numberInt() const;
        /** Retrieve long value for the element safely.  Zero returned if not a number.
         *  Behavior is not defined for double values that are NaNs, or too large/small
         *  to be represented by long longs */
        long long numberLong() const;

        /** Like numberLong() but with well-defined behavior for doubles that
         *  are NaNs, or too large/small to be represented as long longs.
         *  NaNs -> 0
         *  very large doubles -> LLONG_MAX
         *  very small doubles -> LLONG_MIN  */
        long long safeNumberLong() const;

        /** Retrieve the numeric value of the element.  If not of a numeric type, returns 0.
            Note: casts to double, data loss may occur with large (>52 bit) NumberLong values.
        */
        double numberDouble() const;
        /** Retrieve the numeric value of the element.  If not of a numeric type, returns 0.
            Note: casts to double, data loss may occur with large (>52 bit) NumberLong values.
        */
        double number() const { return numberDouble(); }

        /** Retrieve the object ID stored in the object.
            You must ensure the element is of type jstOID first. */
        const mongo::OID &__oid() const { return *reinterpret_cast< const mongo::OID* >( value() ); }

        /** True if element is null. */
        bool isNull() const {
            return type() == jstNULL;
        }

        /** Size (length) of a string element.
            You must assure of type String first.  
            @return string size including terminating null
        */
        int valuestrsize() const {
            return *reinterpret_cast< const int* >( value() );
        }

        // for objects the size *includes* the size of the size field
        size_t objsize() const {
            return static_cast< const size_t >( *reinterpret_cast< const uint32_t* >( value() ) );
        }

        /** Get a string's value.  Also gives you start of the real data for an embedded object.
            You must assure data is of an appropriate type first -- see also valuestrsafe().
        */
        const char * valuestr() const {
            return value() + 4;
        }

        /** Get the string value of the element.  If not a string returns "". */
        const char *valuestrsafe() const {
            return type() == mongo::String ? valuestr() : "";
        }
        /** Get the string value of the element.  If not a string returns "". */
        std::string str() const {
            return type() == mongo::String ? std::string(valuestr(), valuestrsize()-1) : std::string();
        }

        /** Get javascript code of a CodeWScope data element. */
        const char * codeWScopeCode() const {
            massert( 16177 , "not codeWScope" , type() == CodeWScope );
            return value() + 4 + 4; //two ints precede code (see BSON spec)
        }

        /** Get length of the code part of the CodeWScope object
         *  This INCLUDES the null char at the end */
        int codeWScopeCodeLen() const {
            massert( 16178 , "not codeWScope" , type() == CodeWScope );
            return *(int *)( value() + 4 );
        }

        /** Get the scope SavedContext of a CodeWScope data element.
         *
         *  This function is DEPRECATED, since it can error if there are
         *  null chars in the codeWScopeCode. However, some existing indexes
         *  may be based on an incorrect ordering derived from this function,
         *  so it may still need to be used in certain cases.
         *   */
        const char * codeWScopeScopeDataUnsafe() const {
            //This can error if there are null chars in the codeWScopeCode
            return codeWScopeCode() + strlen( codeWScopeCode() ) + 1;
        }

        /* Get the scope SavedContext of a CodeWScope data element.
         *
         * This is the corrected version of codeWScopeScopeDataUnsafe(),
         * but note that existing uses might rely on the behavior of
         * that function so be careful in choosing which version to use.
         */
        const char * codeWScopeScopeData() const {
            return codeWScopeCode() + codeWScopeCodeLen();
        }

        /** Get the embedded object this element holds. */
        BSONObj embeddedObject() const;

        /* uasserts if not an object */
        BSONObj embeddedObjectUserCheck() const;

        BSONObj codeWScopeObject() const;

        /** Get raw binary data.  Element must be of type BinData. Doesn't handle type 2 specially */
        const char *binData(int& len) const {
            // BinData: <int len> <byte subtype> <byte[len] data>
            verify( type() == BinData );
            len = valuestrsize();
            return value() + 5;
        }
        /** Get binary data.  Element must be of type BinData. Handles type 2 */
        const char *binDataClean(int& len) const {
            // BinData: <int len> <byte subtype> <byte[len] data>
            if (binDataType() != ByteArrayDeprecated) {
                return binData(len);
            }
            else {
                // Skip extra size
                len = valuestrsize() - 4;
                return value() + 5 + 4;
            }
        }

        BinDataType binDataType() const {
            // BinData: <int len> <byte subtype> <byte[len] data>
            verify( type() == BinData );
            unsigned char c = (value() + 4)[0];
            return (BinDataType)c;
        }

        /** Retrieve the regex string for a Regex element */
        const char *regex() const {
            verify(type() == RegEx);
            return value();
        }

        /** Retrieve the regex flags (options) for a Regex element */
        const char *regexFlags() const {
            const char *p = regex();
            return p + strlen(p) + 1;
        }

        /** like operator== but doesn't check the fieldname,
            just the value.
        */
        bool valuesEqual(const BSONElement& r) const {
            return woCompare( r , false ) == 0;
        }

        /** Returns true if elements are equal. */
        bool operator==(const BSONElement& r) const {
            return woCompare( r , true ) == 0;
        }
        /** Returns true if elements are unequal. */
        bool operator!=(const BSONElement& r) const { return !operator==(r); }

        /** Well ordered comparison.
            @return <0: l<r. 0:l==r. >0:l>r
            order by type, field name, and field value.
            If considerFieldName is true, pay attention to the field name.
        */
        int woCompare( const BSONElement &e, bool considerFieldName = true ) const;

        const char * rawdata() const { return data; }

        /** 0 == Equality, just not defined yet */
        int getGtLtOp( int def = 0 ) const;

        /** Constructs an empty element */
        BSONElement();

        /** Check that data is internally consistent. */
        void validate() const;

        /** True if this element may contain subobjects. */
        bool mayEncapsulate() const {
            switch ( type() ) {
            case Object:
            case mongo::Array:
            case CodeWScope:
                return true;
            default:
                return false;
            }
        }

        /** True if this element can be a BSONObj */
        bool isABSONObj() const {
            switch( type() ) {
            case Object:
            case mongo::Array:
                return true;
            default:
                return false;
            }
        }

        Date_t timestampTime() const {
            unsigned long long t = ((unsigned int*)(value() + 4 ))[0];
            return t * 1000;
        }
        unsigned int timestampInc() const {
            return ((unsigned int*)(value() ))[0];
        }

        const char * dbrefNS() const {
            uassert( 10063 ,  "not a dbref" , type() == DBRef );
            return value() + 4;
        }

        const mongo::OID& dbrefOID() const {
            uassert( 10064 ,  "not a dbref" , type() == DBRef );
            const char * start = value();
            start += 4 + *reinterpret_cast< const int* >( start );
            return *reinterpret_cast< const mongo::OID* >( start );
        }

        /** this does not use fieldName in the comparison, just the value */
        bool operator<( const BSONElement& other ) const {
            int x = (int)canonicalType() - (int)other.canonicalType();
            if ( x < 0 ) return true;
            else if ( x > 0 ) return false;
            return compareElementValues(*this,other) < 0;
        }

        // @param maxLen don't scan more than maxLen bytes
        explicit BSONElement(const char *d, int maxLen) : data(d) {
            if ( eoo() ) {
                totalSize = 1;
                fieldNameSize_ = 0;
            }
            else {
                totalSize = -1;
                fieldNameSize_ = -1;
                if ( maxLen != -1 ) {
                    int size = (int) strnlen( fieldName(), maxLen - 1 );
                    uassert( 10333 ,  "Invalid field name", size != -1 );
                    fieldNameSize_ = size + 1;
                }
            }
        }

        explicit BSONElement(const char *d) : data(d) {
            fieldNameSize_ = -1;
            totalSize = -1;
            if ( eoo() ) {
                fieldNameSize_ = 0;
                totalSize = 1;
            }
        }

        std::string _asCode() const;
        OpTime _opTime() const;

        template<typename T> bool coerce( T* out ) const;

    private:
        const char *data;
        mutable int fieldNameSize_; // cached value

        mutable int totalSize; /* caches the computed size */

        friend class BSONObjIterator;
        friend class BSONObj;
        const BSONElement& chk(int t) const {
            if ( t != type() ) {
                StringBuilder ss;
                if( eoo() )
                    ss << "field not found, expected type " << t;
                else
                    ss << "wrong type for field (" << fieldName() << ") " << type() << " != " << t;
                msgasserted(13111, ss.str() );
            }
            return *this;
        }
        const BSONElement& chk(bool expr) const {
            massert(13118, "unexpected or missing type value in BSON object", expr);
            return *this;
        }
    };

    inline bool BSONElement::trueValue() const {
        // NOTE Behavior changes must be replicated in Value::coerceToBool().
        switch( type() ) {
        case NumberLong:
            return *reinterpret_cast< const long long* >( value() ) != 0;
        case NumberDouble:
            return (reinterpret_cast < const PackedDouble* >(value ()))->d != 0;
        case NumberInt:
            return *reinterpret_cast< const int* >( value() ) != 0;
        case mongo::Bool:
            return boolean();
        case EOO:
        case jstNULL:
        case Undefined:
            return false;

        default:
            ;
        }
        return true;
    }

    /** @return true if element is of a numeric type. */
    inline bool BSONElement::isNumber() const {
        switch( type() ) {
        case NumberLong:
        case NumberDouble:
        case NumberInt:
            return true;
        default:
            return false;
        }
    }

    inline bool BSONElement::isSimpleType() const {
        switch( type() ) {
        case NumberLong:
        case NumberDouble:
        case NumberInt:
        case mongo::String:
        case mongo::Bool:
        case mongo::Date:
        case jstOID:
            return true;
        default:
            return false;
        }
    }

    inline double BSONElement::numberDouble() const {
        switch( type() ) {
        case NumberDouble:
            return _numberDouble();
        case NumberInt:
            return *reinterpret_cast< const int* >( value() );
        case NumberLong:
            return (double) *reinterpret_cast< const long long* >( value() );
        default:
            return 0;
        }
    }

    /** Retrieve int value for the element safely.  Zero returned if not a number. Converted to int if another numeric type. */
    inline int BSONElement::numberInt() const {
        switch( type() ) {
        case NumberDouble:
            return (int) _numberDouble();
        case NumberInt:
            return _numberInt();
        case NumberLong:
            return (int) _numberLong();
        default:
            return 0;
        }
    }

    /** Retrieve long value for the element safely.  Zero returned if not a number. */
    inline long long BSONElement::numberLong() const {
        switch( type() ) {
        case NumberDouble:
            return (long long) _numberDouble();
        case NumberInt:
            return _numberInt();
        case NumberLong:
            return _numberLong();
        default:
            return 0;
        }
    }

    /** Like numberLong() but with well-defined behavior for doubles that
     *  are NaNs, or too large/small to be represented as long longs.
     *  NaNs -> 0
     *  very large doubles -> LLONG_MAX
     *  very small doubles -> LLONG_MIN  */
    inline long long BSONElement::safeNumberLong() const {
        double d;
        switch( type() ) {
        case NumberDouble:
            d = numberDouble();
            if ( isNaN( d ) ){
                return 0;
            }
            if ( d > (double) std::numeric_limits<long long>::max() ){
                return std::numeric_limits<long long>::max();
            }
            if ( d < std::numeric_limits<long long>::min() ){
                return std::numeric_limits<long long>::min();
            }
        default:
            return numberLong();
        }
    }

    inline BSONElement::BSONElement() {
        static char z = 0;
        data = &z;
        fieldNameSize_ = 0;
        totalSize = 1;
    }

}
