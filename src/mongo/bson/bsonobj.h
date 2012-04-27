// @file bsonobj.h

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

#include <boost/noncopyable.hpp>
#include <boost/intrusive_ptr.hpp>
#include <set>
#include <list>
#include <vector>
#include "util/atomic_int.h"
#include "util/builder.h"
#include "stringdata.h"

namespace mongo {

    typedef std::set< BSONElement, BSONElementCmpWithoutField > BSONElementSet;
    typedef std::multiset< BSONElement, BSONElementCmpWithoutField > BSONElementMSet;

    /**
       C++ representation of a "BSON" object -- that is, an extended JSON-style
       object in a binary representation.

       See bsonspec.org.

       Note that BSONObj's have a smart pointer capability built in -- so you can
       pass them around by value.  The reference counts used to implement this
       do not use locking, so copying and destroying BSONObj's are not thread-safe
       operations.

     BSON object format:

     code
     <unsigned totalSize> {<byte BSONType><cstring FieldName><Data>}* EOO

     totalSize includes itself.

     Data:
     Bool:      <byte>
     EOO:       nothing follows
     Undefined: nothing follows
     OID:       an OID object
     NumberDouble: <double>
     NumberInt: <int32>
     String:    <unsigned32 strsizewithnull><cstring>
     Date:      <8bytes>
     Regex:     <cstring regex><cstring options>
     Object:    a nested object, leading with its entire size, which terminates with EOO.
     Array:     same as object
     DBRef:     <strlen> <cstring ns> <oid>
     DBRef:     a database reference: basically a collection name plus an Object ID
     BinData:   <int len> <byte subtype> <byte[len] data>
     Code:      a function (not a closure): same format as String.
     Symbol:    a language symbol (say a python symbol).  same format as String.
     Code With Scope: <total size><String><Object>
     \endcode
     */
    class BSONObj {
    public:

        /** Construct a BSONObj from data in the proper format.
         *  Use this constructor when something else owns msgdata's buffer
        */
        explicit BSONObj(const char *msgdata) {
            init(msgdata);
        }

        /** Construct a BSONObj from data in the proper format.
         *  Use this constructor when you want BSONObj to free(holder) when it is no longer needed
         *  BSONObj::Holder has an extra 4 bytes for a ref-count before the start of the object
        */
        class Holder;
        explicit BSONObj(Holder* holder) {
            init(holder);
        }

        explicit BSONObj(const Record *r);

        /** Construct an empty BSONObj -- that is, {}. */
        BSONObj();

        ~BSONObj() { 
            _objdata = 0; // defensive
        }

        /**
           A BSONObj can use a buffer it "owns" or one it does not.

           OWNED CASE
           If the BSONObj owns the buffer, the buffer can be shared among several BSONObj's (by assignment).
           In this case the buffer is basically implemented as a shared_ptr.
           Since BSONObj's are typically immutable, this works well.

           UNOWNED CASE
           A BSONObj can also point to BSON data in some other data structure it does not "own" or free later.
           For example, in a memory mapped file.  In this case, it is important the original data stays in
           scope for as long as the BSONObj is in use.  If you think the original data may go out of scope,
           call BSONObj::getOwned() to promote your BSONObj to having its own copy.

           On a BSONObj assignment, if the source is unowned, both the source and dest will have unowned
           pointers to the original buffer after the assignment.

           If you are not sure about ownership but need the buffer to last as long as the BSONObj, call
           getOwned().  getOwned() is a no-op if the buffer is already owned.  If not already owned, a malloc
           and memcpy will result.

           Most ways to create BSONObj's create 'owned' variants.  Unowned versions can be created with:
           (1) specifying true for the ifree parameter in the constructor
           (2) calling BSONObjBuilder::done().  Use BSONObjBuilder::obj() to get an owned copy
           (3) retrieving a subobject retrieves an unowned pointer into the parent BSON object

           @return true if this is in owned mode
        */
        bool isOwned() const { return _holder.get() != 0; }

        /** assure the data buffer is under the control of this BSONObj and not a remote buffer 
            @see isOwned()
        */
        BSONObj getOwned() const;

        /** @return a new full (and owned) copy of the object. */
        BSONObj copy() const;

        /** Readable representation of a BSON object in an extended JSON-style notation.
            This is an abbreviated representation which might be used for logging.
        */
        enum { maxToStringRecursionDepth = 100 };

        std::string toString( bool isArray = false, bool full=false ) const;
        void toString( StringBuilder& s, bool isArray = false, bool full=false, int depth=0 ) const;

        /** Properly formatted JSON string.
            @param pretty if true we try to add some lf's and indentation
        */
        std::string jsonString( JsonStringFormat format = Strict, int pretty = 0 ) const;

        /** note: addFields always adds _id even if not specified */
        int addFields(BSONObj& from, std::set<std::string>& fields); /* returns n added */

        /** remove specified field and return a new object with the remaining fields.
            slowish as builds a full new object
         */
        BSONObj removeField(const StringData& name) const;

        /** returns # of top level fields in the object
           note: iterates to count the fields
        */
        int nFields() const;

        /** adds the field names to the fields set.  does NOT clear it (appends). */
        int getFieldNames(std::set<std::string>& fields) const;

        /** @return the specified element.  element.eoo() will be true if not found.
            @param name field to find. supports dot (".") notation to reach into embedded objects.
             for example "x.y" means "in the nested object in field x, retrieve field y"
        */
        BSONElement getFieldDotted(const char *name) const;
        /** @return the specified element.  element.eoo() will be true if not found.
            @param name field to find. supports dot (".") notation to reach into embedded objects.
             for example "x.y" means "in the nested object in field x, retrieve field y"
        */
        BSONElement getFieldDotted(const std::string& name) const {
            return getFieldDotted( name.c_str() );
        }

        /** Like getFieldDotted(), but expands arrays and returns all matching objects.
         *  Turning off expandLastArray allows you to retrieve nested array objects instead of
         *  their contents.
         */
        void getFieldsDotted(const StringData& name, BSONElementSet &ret, bool expandLastArray = true ) const;
        void getFieldsDotted(const StringData& name, BSONElementMSet &ret, bool expandLastArray = true ) const;

        /** Like getFieldDotted(), but returns first array encountered while traversing the
            dotted fields of name.  The name variable is updated to represent field
            names with respect to the returned element. */
        BSONElement getFieldDottedOrArray(const char *&name) const;

        /** Get the field of the specified name. eoo() is true on the returned
            element if not found.
        */
        BSONElement getField(const StringData& name) const;

        /** Get several fields at once. This is faster than separate getField() calls as the size of 
            elements iterated can then be calculated only once each.
            @param n number of fieldNames, and number of elements in the fields array
            @param fields if a field is found its element is stored in its corresponding position in this array.
                   if not found the array element is unchanged.
         */
        void getFields(unsigned n, const char **fieldNames, BSONElement *fields) const;

        /** Get the field of the specified name. eoo() is true on the returned
            element if not found.
        */
        BSONElement operator[] (const char *field) const {
            return getField(field);
        }

        BSONElement operator[] (const std::string& field) const {
            return getField(field);
        }

        BSONElement operator[] (int field) const {
            StringBuilder ss;
            ss << field;
            std::string s = ss.str();
            return getField(s.c_str());
        }

        /** @return true if field exists */
        bool hasField( const char * name ) const { return !getField(name).eoo(); }
        /** @return true if field exists */
        bool hasElement(const char *name) const { return hasField(name); }

        /** @return "" if DNE or wrong type */
        const char * getStringField(const char *name) const;

        /** @return subobject of the given name */
        BSONObj getObjectField(const char *name) const;

        /** @return INT_MIN if not present - does some type conversions */
        int getIntField(const char *name) const;

        /** @return false if not present 
            @see BSONElement::trueValue()
         */
        bool getBoolField(const char *name) const;

        /**
           sets element field names to empty string
           If a field in pattern is missing, it is omitted from the returned
           object.
        */
        BSONObj extractFieldsUnDotted(BSONObj pattern) const;

        /** extract items from object which match a pattern object.
            e.g., if pattern is { x : 1, y : 1 }, builds an object with
            x and y elements of this object, if they are present.
           returns elements with original field names
        */
        BSONObj extractFields(const BSONObj &pattern , bool fillWithNull=false) const;

        BSONObj filterFieldsUndotted(const BSONObj &filter, bool inFilter) const;

        BSONElement getFieldUsingIndexNames(const char *fieldName, const BSONObj &indexKey) const;

        /** arrays are bson objects with numeric and increasing field names
            @return true if field names are numeric and increasing
         */
        bool couldBeArray() const;

        /** @return the raw data of the object */
        const char *objdata() const {
            return _objdata;
        }
        /** @return total size of the BSON object in bytes */
        int objsize() const { return little<int>::ref(objdata()); }

        /** performs a cursory check on the object's size only. */
        bool isValid() const;

        /** @return if the user is a valid user doc
            criter: isValid() no . or $ field names
         */
        bool okForStorage() const;

        /** @return true if object is empty -- i.e.,  {} */
        bool isEmpty() const { return objsize() <= 5; }

        void dump() const;

        /** Alternative output format */
        std::string hexDump() const;

        /**wo='well ordered'.  fields must be in same order in each object.
           Ordering is with respect to the signs of the elements
           and allows ascending / descending key mixing.
           @return  <0 if l<r. 0 if l==r. >0 if l>r
        */
        int woCompare(const BSONObj& r, const Ordering &o,
                      bool considerFieldName=true) const;

        /**wo='well ordered'.  fields must be in same order in each object.
           Ordering is with respect to the signs of the elements
           and allows ascending / descending key mixing.
           @return  <0 if l<r. 0 if l==r. >0 if l>r
        */
        int woCompare(const BSONObj& r, const BSONObj &ordering = BSONObj(),
                      bool considerFieldName=true) const;

        bool operator<( const BSONObj& other ) const { return woCompare( other ) < 0; }
        bool operator<=( const BSONObj& other ) const { return woCompare( other ) <= 0; }
        bool operator>( const BSONObj& other ) const { return woCompare( other ) > 0; }
        bool operator>=( const BSONObj& other ) const { return woCompare( other ) >= 0; }

        /**
         * @param useDotted whether to treat sort key fields as possibly dotted and expand into them
         */
        int woSortOrder( const BSONObj& r , const BSONObj& sortKey , bool useDotted=false ) const;

        bool equal(const BSONObj& r) const;

        /** This is "shallow equality" -- ints and doubles won't match.  for a
           deep equality test use woCompare (which is slower).
        */
        bool binaryEqual(const BSONObj& r) const {
            int os = objsize();
            if ( os == r.objsize() ) {
                return (os == 0 || memcmp(objdata(),r.objdata(),os)==0);
            }
            return false;
        }

        /** @return first field of the object */
        BSONElement firstElement() const { return BSONElement(objdata() + 4); }

        /** faster than firstElement().fieldName() - for the first element we can easily find the fieldname without 
            computing the element size.
        */
        const char * firstElementFieldName() const { 
            const char *p = objdata() + 4;
            return *p == EOO ? "" : p+1;
        }

        BSONType firstElementType() const { 
            const char *p = objdata() + 4;
            return (BSONType) *p;
        }

        /** Get the _id field from the object.  For good performance drivers should
            assure that _id is the first element of the object; however, correct operation
            is assured regardless.
            @return true if found
        */
        bool getObjectID(BSONElement& e) const;

        /** @return A hash code for the object */
        int hash() const {
            unsigned x = 0;
            const signed char *p = (signed char*)objdata();
            for ( int i = 0, n = objsize(); i < n; i++ )
                x = x * 131 + p[i];
            return (x & 0x7fffffff) | 0x8000000; // must be > 0
        }

        // Return a version of this object where top level elements of types
        // that are not part of the bson wire protocol are replaced with
        // string identifier equivalents.
        // TODO Support conversion of element types other than min and max.
        BSONObj clientReadable() const;

        /** Return new object with the field names replaced by those in the
            passed object. */
        BSONObj replaceFieldNames( const BSONObj &obj ) const;

        /** true unless corrupt */
        bool valid() const;

        /** @return an md5 value for this object. */
        std::string md5() const;

        bool operator==( const BSONObj& other ) const { return equal( other ); }
        bool operator!=(const BSONObj& other) const { return !operator==( other); }

        enum MatchType {
            Equality = 0,
            LT = 0x1,
            LTE = 0x3,
            GTE = 0x6,
            GT = 0x4,
            opIN = 0x8, // { x : { $in : [1,2,3] } }
            NE = 0x9,
            opSIZE = 0x0A,
            opALL = 0x0B,
            NIN = 0x0C,
            opEXISTS = 0x0D,
            opMOD = 0x0E,
            opTYPE = 0x0F,
            opREGEX = 0x10,
            opOPTIONS = 0x11,
            opELEM_MATCH = 0x12,
            opNEAR = 0x13,
            opWITHIN = 0x14,
            opMAX_DISTANCE=0x15
        };

        /** add all elements of the object to the specified vector */
        void elems(std::vector<BSONElement> &) const;
        /** add all elements of the object to the specified list */
        void elems(std::list<BSONElement> &) const;

        /** add all values of the object to the specified vector.  If type mismatches, exception.
            this is most useful when the BSONObj is an array, but can be used with non-arrays too in theory.

            example:
              bo sub = y["subobj"].Obj();
              std::vector<int> myints;
              sub.Vals(myints);
        */
        template <class T>
        void Vals(std::vector<T> &) const;
        /** add all values of the object to the specified list.  If type mismatches, exception. */
        template <class T>
        void Vals(std::list<T> &) const;

        /** add all values of the object to the specified vector.  If type mismatches, skip. */
        template <class T>
        void vals(std::vector<T> &) const;
        /** add all values of the object to the specified list.  If type mismatches, skip. */
        template <class T>
        void vals(std::list<T> &) const;

        friend class BSONObjIterator;
        typedef BSONObjIterator iterator;

        /** use something like this:
            for( BSONObj::iterator i = myObj.begin(); i.more(); ) {
                BSONElement e = i.next();
                ...
            }
        */
        BSONObjIterator begin() const;

        void appendSelfToBufBuilder(BufBuilder& b) const {
            verify( objsize() );
            b.appendBuf(reinterpret_cast<const void *>( objdata() ), objsize());
        }

#pragma pack(1)
        class Holder : boost::noncopyable {
        private:
            Holder(); // this class should never be explicitly created
            AtomicUInt refCount;
        public:
            char data[4]; // start of object

            void zero() { refCount.zero(); }

            // these are called automatically by boost::intrusive_ptr
            friend void intrusive_ptr_add_ref(Holder* h) { h->refCount++; }
            friend void intrusive_ptr_release(Holder* h) {
#if defined(_DEBUG) // cant use dassert or DEV here
                verify((int)h->refCount > 0); // make sure we haven't already freed the buffer
#endif
                if(--(h->refCount) == 0){
#if defined(_DEBUG)
                    unsigned sz = (unsigned&) *h->data;
                    verify(sz < BSONObjMaxInternalSize * 3);
                    memset(h->data, 0xdd, sz);
#endif
                    free(h);
                }
            }
        };
#pragma pack()

    BSONObj(const BSONObj &rO):
        _objdata(rO._objdata), _holder(rO._holder) {
        }

    BSONObj &operator=(const BSONObj &rRHS) {
        if (this != &rRHS) {
            _objdata = rRHS._objdata;
            _holder = rRHS._holder;
        }
        return *this;
    }

    private:
        const char *_objdata;
        boost::intrusive_ptr< Holder > _holder;

        void _assertInvalid() const;

        void init(Holder *holder) {
            _holder = holder; // holder is now managed by intrusive_ptr
            init(holder->data);
        }
        void init(const char *data) {
            _objdata = data;
            if ( !isValid() )
                _assertInvalid();
        }
    };

    std::ostream& operator<<( std::ostream &s, const BSONObj &o );
    std::ostream& operator<<( std::ostream &s, const BSONElement &e );

    StringBuilder& operator<<( StringBuilder &s, const BSONObj &o );
    StringBuilder& operator<<( StringBuilder &s, const BSONElement &e );


    struct BSONArray : BSONObj {
        // Don't add anything other than forwarding constructors!!!
        BSONArray(): BSONObj() {}
        explicit BSONArray(const BSONObj& obj): BSONObj(obj) {}
    };

}
