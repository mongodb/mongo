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

#include <set>
#include <list>
#include <vector>
#include "util/builder.h"
#include "stringdata.h"

namespace mongo {

    typedef set< BSONElement, BSONElementCmpWithoutField > BSONElementSet;

    const int BSONObjMaxSize = 32 * 1024 * 1024;

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
            @param ifree true if the BSONObj should free() the msgdata when 
            it destructs. 
            */
        explicit BSONObj(const char *msgdata, bool ifree = false) {
            init(msgdata, ifree);
        }
        BSONObj(const Record *r);
        /** Construct an empty BSONObj -- that is, {}. */
        BSONObj();
        // defensive
        ~BSONObj() { _objdata = 0; }

        void appendSelfToBufBuilder(BufBuilder& b) const {
            assert( objsize() );
            b.appendBuf(reinterpret_cast<const void *>( objdata() ), objsize());
        }

        /** Readable representation of a BSON object in an extended JSON-style notation. 
            This is an abbreviated representation which might be used for logging.
        */
        string toString( bool isArray = false, bool full=false ) const;
        void toString(StringBuilder& s, bool isArray = false, bool full=false ) const;
        
        /** Properly formatted JSON string. 
            @param pretty if true we try to add some lf's and indentation
        */
        string jsonString( JsonStringFormat format = Strict, int pretty = 0 ) const;

        /** note: addFields always adds _id even if not specified */
        int addFields(BSONObj& from, set<string>& fields); /* returns n added */

        /** returns # of top level fields in the object
           note: iterates to count the fields
        */
        int nFields() const;

        /** adds the field names to the fields set.  does NOT clear it (appends). */
        int getFieldNames(set<string>& fields) const;

        /** return has eoo() true if no match
           supports "." notation to reach into embedded objects
        */
        BSONElement getFieldDotted(const char *name) const;
        /** return has eoo() true if no match
           supports "." notation to reach into embedded objects
        */
        BSONElement getFieldDotted(const string& name) const {
            return getFieldDotted( name.c_str() );
        }

        /** Like getFieldDotted(), but expands multikey arrays and returns all matching objects
         */
        void getFieldsDotted(const StringData& name, BSONElementSet &ret ) const;
        /** Like getFieldDotted(), but returns first array encountered while traversing the
            dotted fields of name.  The name variable is updated to represent field
            names with respect to the returned element. */
        BSONElement getFieldDottedOrArray(const char *&name) const;

        /** Get the field of the specified name. eoo() is true on the returned 
            element if not found. 
        */
        BSONElement getField(const StringData& name) const;

        /** Get the field of the specified name. eoo() is true on the returned 
            element if not found. 
        */
        BSONElement operator[] (const char *field) const { 
            return getField(field);
        }

        BSONElement operator[] (const string& field) const { 
            return getField(field);
        }

        BSONElement operator[] (int field) const { 
            StringBuilder ss;
            ss << field;
            string s = ss.str();
            return getField(s.c_str());
        }

		/** @return true if field exists */
        bool hasField( const char * name )const {
            return ! getField( name ).eoo();
        }

        /** @return "" if DNE or wrong type */
        const char * getStringField(const char *name) const;

		/** @return subobject of the given name */
        BSONObj getObjectField(const char *name) const;

        /** @return INT_MIN if not present - does some type conversions */
        int getIntField(const char *name) const;

        /** @return false if not present */
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
        
        /** @return the raw data of the object */
        const char *objdata() const {
            return _objdata;
        }
        /** @return total size of the BSON object in bytes */
        int objsize() const {
            return *(reinterpret_cast<const int*>(objdata()));
        }

        /** performs a cursory check on the object's size only. */
        bool isValid();

        /** @return if the user is a valid user doc
            criter: isValid() no . or $ field names
         */
        bool okForStorage() const;

		/** @return true if object is empty -- i.e.,  {} */
        bool isEmpty() const {
            return objsize() <= 5;
        }

        void dump() const;

        /** Alternative output format */
        string hexDump() const;
        
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

        /** This is "shallow equality" -- ints and doubles won't match.  for a
           deep equality test use woCompare (which is slower).
        */
        bool woEqual(const BSONObj& r) const {
            int os = objsize();
            if ( os == r.objsize() ) {
                return (os == 0 || memcmp(objdata(),r.objdata(),os)==0);
            }
            return false;
        }

		/** @return first field of the object */
        BSONElement firstElement() const {
            return BSONElement(objdata() + 4);
        }

		/** @return true if field exists in the object */
        bool hasElement(const char *name) const;

		/** Get the _id field from the object.  For good performance drivers should 
            assure that _id is the first element of the object; however, correct operation 
            is assured regardless.
            @return true if found
		*/
		bool getObjectID(BSONElement& e) const;

        /** makes a copy of the object. */
        BSONObj copy() const;

        /* make sure the data buffer is under the control of this BSONObj and not a remote buffer */
        BSONObj getOwned() const{
            if ( !isOwned() )
                return copy();
            return *this;
        }
        bool isOwned() const { return _holder.get() != 0; }

        /** @return A hash code for the object */
        int hash() const {
            unsigned x = 0;
            const char *p = objdata();
            for ( int i = 0; i < objsize(); i++ )
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
        string md5() const;
        
        bool operator==( const BSONObj& other ) const{
            return woCompare( other ) == 0;
        }

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
        void elems(vector<BSONElement> &) const;
        /** add all elements of the object to the specified list */
        void elems(list<BSONElement> &) const;

        /** add all values of the object to the specified vector.  If type mismatches, exception. */
        template <class T>
        void Vals(vector<T> &) const;
        /** add all values of the object to the specified list.  If type mismatches, exception. */
        template <class T>
        void Vals(list<T> &) const;

        /** add all values of the object to the specified vector.  If type mismatches, skip. */
        template <class T>
        void vals(vector<T> &) const;
        /** add all values of the object to the specified list.  If type mismatches, skip. */
        template <class T>
        void vals(list<T> &) const;

        friend class BSONObjIterator;
        typedef BSONObjIterator iterator;
        BSONObjIterator begin();

private:
        class Holder {
        public:
            Holder( const char *objdata ) :
            _objdata( objdata ) {
            }
            ~Holder() {
                free((void *)_objdata);
                _objdata = 0;
            }
        private:
            const char *_objdata;
        };
        const char *_objdata;
        boost::shared_ptr< Holder > _holder;
        void init(const char *data, bool ifree) {
            if ( ifree )
                _holder.reset( new Holder( data ) );
            _objdata = data;
            if ( ! isValid() ){
                StringBuilder ss;
                int os = objsize();
                ss << "Invalid BSONObj spec size: " << os << " (" << toHex( &os, 4 ) << ")";
                try {
                    BSONElement e = firstElement();
                    ss << " first element:" << e.toString() << " ";
                }
                catch ( ... ){}
                string s = ss.str();
                massert( 10334 , s , 0 );
            }
        }
    };
    ostream& operator<<( ostream &s, const BSONObj &o );
    ostream& operator<<( ostream &s, const BSONElement &e );

    struct BSONArray : BSONObj {
        // Don't add anything other than forwarding constructors!!!
        BSONArray(): BSONObj() {}
        explicit BSONArray(const BSONObj& obj): BSONObj(obj) {}
    };

}
