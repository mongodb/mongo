/** @file jsobj.h 
    BSON classes
*/

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

/**
   BSONObj and its helpers

   "BSON" stands for "binary JSON" -- ie a binary way to represent objects that would be
   represented in JSON (plus a few extensions useful for databases & other languages).

   http://www.mongodb.org/display/DOCS/BSON
*/

#pragma once

#include "../stdafx.h"
#include "../util/builder.h"
#include "../util/optime.h"
#include "boost/utility.hpp"
#include <set>

namespace mongo {

    class BSONArrayBuilder;
    class BSONElement;
    class BSONObj;
    class BSONObjBuilder;
    class BSONObjBuilderValueStream;
    class BSONObjIterator;
    class Ordering;
    class Record;
    struct BSONArray; // empty subclass of BSONObj useful for overloading

    extern BSONObj maxKey;
    extern BSONObj minKey;
}

#include "../bson/bsontypes.h"
#include "../bson/oid.h"
#include "../bson/bsonelement.h"

namespace mongo {

    int getGtLtOp(const BSONElement& e);

    struct BSONElementCmpWithoutField {
        bool operator()( const BSONElement &l, const BSONElement &r ) const {
            return l.woCompare( r, false ) < 0;
        }
    };
    
    typedef set< BSONElement, BSONElementCmpWithoutField > BSONElementSet;

    /**
	   C++ representation of a "BSON" object -- that is, an extended JSON-style 
       object in a binary representation.

       Note that BSONObj's have a smart pointer capability built in -- so you can 
       pass them around by value.  The reference counts used to implement this
       do not use locking, so copying and destroying BSONObj's are not thread-safe
       operations.

     BSON object format:
     
     \code
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
        BSONObj() : _objdata( reinterpret_cast< const char * >( &emptyObject ) ) { }
        // defensive
        ~BSONObj() { _objdata = 0; }

        void appendSelfToBufBuilder(BufBuilder& b) const {
            assert( objsize() );
            b.append(reinterpret_cast<const void *>( objdata() ), objsize());
        }

        /** Readable representation of a BSON object in an extended JSON-style notation. 
            This is an abbreviated representation which might be used for logging.
        */
        string toString( bool isArray = false ) const;
        operator string() const { return toString(); }
        
        /** Properly formatted JSON string. 
            @param pretty if tru1 we try to add some lf's and indentation
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
        /** Like getFieldDotted(), but expands multikey arrays and returns all matching objects
         */
        void getFieldsDotted(const char *name, BSONElementSet &ret ) const;
        /** Like getFieldDotted(), but returns first array encountered while traversing the
            dotted fields of name.  The name variable is updated to represent field
            names with respect to the returned element. */
        BSONElement getFieldDottedOrArray(const char *&name) const;

        /** Get the field of the specified name. eoo() is true on the returned 
            element if not found. 
        */
        BSONElement getField(const char *name) const;

        /** Get the field of the specified name. eoo() is true on the returned 
            element if not found. 
        */
        BSONElement getField(const string name) const {
            return getField( name.c_str() );
        };

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
            stringstream ss;
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

        /** makes a new BSONObj with the fields specified in pattern.
           fields returned in the order they appear in pattern.
           if any field is missing or undefined in the object, that field in the
           output will be null.

           sets output field names to match pattern field names.
           If an array is encountered while scanning the dotted names in pattern,
           that field is treated as missing.
        */
        BSONObj extractFieldsDotted(BSONObj pattern) const;
        
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

        void dump() const {
            out() << hex;
            const char *p = objdata();
            for ( int i = 0; i < objsize(); i++ ) {
                out() << i << '\t' << ( 0xff & ( (unsigned) *p ) );
                if ( *p >= 'A' && *p <= 'z' )
                    out() << '\t' << *p;
                out() << endl;
                p++;
            }
        }

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
        
        int woSortOrder( const BSONObj& r , const BSONObj& sortKey ) const;

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
        
        bool operator==( const BSONObj& other ){
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

private:
        friend class BSONObjIterator;
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
                stringstream ss;
                ss << "Invalid BSONObj spec size: " << objsize();
                try {
                    BSONElement e = firstElement();
                    ss << " first element:" << e.toString() << " ";
                }
                catch ( ... ){}
                string s = ss.str();
                massert( 10334 ,  s , 0 );
            }
        }
#pragma pack(1)
        static struct EmptyObject {
            EmptyObject() {
                len = 5;
                jstype = EOO;
            }
            int len;
            char jstype;
        } emptyObject;
#pragma pack()
    };
    ostream& operator<<( ostream &s, const BSONObj &o );
    ostream& operator<<( ostream &s, const BSONElement &e );

    struct BSONArray : BSONObj {
        // Don't add anything other than forwarding constructors!!!
        BSONArray(): BSONObj() {}
        explicit BSONArray(const BSONObj& obj): BSONObj(obj) {}
    };

    class BSONObjCmp {
    public:
        BSONObjCmp( const BSONObj &_order = BSONObj() ) : order( _order ) {}
        bool operator()( const BSONObj &l, const BSONObj &r ) const {
            return l.woCompare( r, order ) < 0;
        }
    private:
        BSONObj order;
    };

    class BSONObjCmpDefaultOrder : public BSONObjCmp {
    public:
        BSONObjCmpDefaultOrder() : BSONObjCmp( BSONObj() ) {}
    };

    typedef set< BSONObj, BSONObjCmpDefaultOrder > BSONObjSetDefaultOrder;

    enum FieldCompareResult {
        LEFT_SUBFIELD = -2,
        LEFT_BEFORE = -1,
        SAME = 0,
        RIGHT_BEFORE = 1 ,
        RIGHT_SUBFIELD = 2
    };

    FieldCompareResult compareDottedFieldNames( const string& l , const string& r );

/** Use BSON macro to build a BSONObj from a stream 

    e.g., 
       BSON( "name" << "joe" << "age" << 33 )

    with auto-generated object id:
       BSON( GENOID << "name" << "joe" << "age" << 33 )
 
    The labels GT, GTE, LT, LTE, NE can be helpful for stream-oriented construction
    of a BSONObj, particularly when assembling a Query.  For example,
    BSON( "a" << GT << 23.4 << NE << 30 << "b" << 2 ) produces the object
    { a: { \$gt: 23.4, \$ne: 30 }, b: 2 }.
*/
#define BSON(x) (( mongo::BSONObjBuilder(64) << x ).obj())

/** Use BSON_ARRAY macro like BSON macro, but without keys

    BSONArray arr = BSON_ARRAY( "hello" << 1 << BSON( "foo" << BSON_ARRAY( "bar" << "baz" << "qux" ) ) );

 */
#define BSON_ARRAY(x) (( mongo::BSONArrayBuilder() << x ).arr())

    /* Utility class to auto assign object IDs.
       Example:
         cout << BSON( GENOID << "z" << 3 ); // { _id : ..., z : 3 }
    */
    extern struct IDLabeler { } GENOID;

    /* Utility class to add a Date element with the current time
       Example: 
         cout << BSON( "created" << DATENOW ); // { created : "2009-10-09 11:41:42" }
    */
    extern struct DateNowLabeler { } DATENOW;

    // Utility class to implement GT, GTE, etc as described above.
    class Labeler {
    public:
        struct Label {
            Label( const char *l ) : l_( l ) {}
            const char *l_;
        };
        Labeler( const Label &l, BSONObjBuilderValueStream *s ) : l_( l ), s_( s ) {}
        template<class T>
        BSONObjBuilder& operator<<( T value );

        /* the value of the element e is appended i.e. for 
             "age" << GT << someElement
           one gets 
             { age : { $gt : someElement's value } } 
        */
        BSONObjBuilder& operator<<( const BSONElement& e );
    private:
        const Label &l_;
        BSONObjBuilderValueStream *s_;
    };
    
    extern Labeler::Label GT;
    extern Labeler::Label GTE;
    extern Labeler::Label LT;
    extern Labeler::Label LTE;
    extern Labeler::Label NE;
    extern Labeler::Label SIZE;
    
    // Utility class to implement BSON( key << val ) as described above.
    class BSONObjBuilderValueStream : public boost::noncopyable {
    public:
        friend class Labeler;
        BSONObjBuilderValueStream( BSONObjBuilder * builder );

        BSONObjBuilder& operator<<( const BSONElement& e );
        
        template<class T> 
        BSONObjBuilder& operator<<( T value );

        BSONObjBuilder& operator<<(DateNowLabeler& id);
        
        Labeler operator<<( const Labeler::Label &l );

        void endField( const char *nextFieldName = 0 );
        bool subobjStarted() const { return _fieldName != 0; }
        
    private:
        const char * _fieldName;
        BSONObjBuilder * _builder;

        bool haveSubobj() const { return _subobj.get() != 0; }
        BSONObjBuilder *subobj();
        auto_ptr< BSONObjBuilder > _subobj;
    };
    
    /**
       used in conjuction with BSONObjBuilder, allows for proper buffer size to prevent crazy memory usage
     */
    class BSONSizeTracker {
    public:
#define BSONSizeTrackerSize 10

        BSONSizeTracker(){
            _pos = 0;
            for ( int i=0; i<BSONSizeTrackerSize; i++ )
                _sizes[i] = 512; // this is the default, so just be consistent
        }
        
        ~BSONSizeTracker(){
        }
        
        void got( int size ){
            _sizes[_pos++] = size;
            if ( _pos >= BSONSizeTrackerSize )
                _pos = 0;
        }
        
        /**
         * right now choosing largest size
         */
        int getSize() const {
            int x = 16; // sane min
            for ( int i=0; i<BSONSizeTrackerSize; i++ ){
                if ( _sizes[i] > x )
                    x = _sizes[i];
            }
            return x;
        }
        
    private:
        int _pos;
        int _sizes[BSONSizeTrackerSize];
    };

}

#include "../bson/bsonobjbuilder.h"

namespace mongo {
    
    /** iterator for a BSONObj

       Note each BSONObj ends with an EOO element: so you will get more() on an empty
       object, although next().eoo() will be true.

       todo: we may want to make a more stl-like iterator interface for this
             with things like begin() and end()
    */
    class BSONObjIterator {
    public:
        /** Create an iterator for a BSON object. 
        */
        BSONObjIterator(const BSONObj& jso) {
            int sz = jso.objsize();
            if ( sz == 0 ) {
                _pos = _theend = 0;
                return;
            }
            _pos = jso.objdata() + 4;
            _theend = jso.objdata() + sz;
        }
        
        BSONObjIterator( const char * start , const char * end ){
            _pos = start + 4;
            _theend = end;
        }
        
        /** @return true if more elements exist to be enumerated. */
        bool moreWithEOO() {
            return _pos < _theend;
        }
        bool more(){
            return _pos < _theend && _pos[0];
        }
        /** @return the next element in the object. For the final element, element.eoo() will be true. */
        BSONElement next( bool checkEnd = false ) {
            assert( _pos < _theend );
            BSONElement e( _pos, checkEnd ? (int)(_theend - _pos) : -1 );
            _pos += e.size( checkEnd ? (int)(_theend - _pos) : -1 );
            return e;
        }
    private:
        const char* _pos;
        const char* _theend;
    };

    /* iterator a BSONObj which is an array, in array order.
    class JSArrayIter {
    public:
    	BSONObjIterator(const BSONObj& jso) {
    ...
    	}
    	bool more() { return ... }
    	BSONElement next() {
    ...
    	}
    };
    */
	
    // a BoundList contains intervals specified by inclusive start
    // and end bounds.  The intervals should be nonoverlapping and occur in
    // the specified direction of traversal.  For example, given a simple index {i:1}
    // and direction +1, one valid BoundList is: (1, 2); (4, 6).  The same BoundList
    // would be valid for index {i:-1} with direction -1.
    typedef vector< pair< BSONObj, BSONObj > > BoundList;	

    /*- just for testing -- */

#pragma pack(1)
    struct JSObj1 {
        JSObj1() {
            totsize=sizeof(JSObj1);
            n = NumberDouble;
            strcpy_s(nname, 5, "abcd");
            N = 3.1;
            s = String;
            strcpy_s(sname, 7, "abcdef");
            slen = 10;
            strcpy_s(sval, 10, "123456789");
            eoo = EOO;
        }
        unsigned totsize;

        char n;
        char nname[5];
        double N;

        char s;
        char sname[7];
        unsigned slen;
        char sval[10];

        char eoo;
    };
#pragma pack()

    extern JSObj1 js1;

#ifdef _DEBUG
#define CHECK_OBJECT( o , msg ) massert( 10337 ,  (string)"object not valid" + (msg) , (o).isValid() )
#else
#define CHECK_OBJECT( o , msg )
#endif

    inline BSONObj BSONElement::embeddedObjectUserCheck() const {
        uassert( 10065 ,  "invalid parameter: expected an object", isABSONObj() );
        return BSONObj(value());
    }

    inline BSONObj BSONElement::embeddedObject() const {
        assert( isABSONObj() );
        return BSONObj(value());
    }

    inline BSONObj BSONElement::codeWScopeObject() const {
        assert( type() == CodeWScope );
        int strSizeWNull = *(int *)( value() + 4 );
        return BSONObj( value() + 4 + 4 + strSizeWNull );
    }
    
    inline BSONObj BSONObj::copy() const {
        char *p = (char*) malloc(objsize());
        memcpy(p, objdata(), objsize());
        return BSONObj(p, true);
    }

    // wrap this element up as a singleton object.
    inline BSONObj BSONElement::wrap() const {
        BSONObjBuilder b(size()+6);
        b.append(*this);
        return b.obj();
    }

    inline BSONObj BSONElement::wrap( const char * newName ) const {
        BSONObjBuilder b(size()+6+strlen(newName));
        b.appendAs(*this,newName);
        return b.obj();
    }


    inline bool BSONObj::hasElement(const char *name) const {
        if ( !isEmpty() ) {
            BSONObjIterator it(*this);
            while ( it.moreWithEOO() ) {
                BSONElement e = it.next();
                if ( strcmp(name, e.fieldName()) == 0 )
                    return true;
            }
        }
        return false;
    }

    inline BSONElement BSONObj::getField(const char *name) const {
        BSONObjIterator i(*this);
        while ( i.more() ) {
            BSONElement e = i.next();
            if ( strcmp(e.fieldName(), name) == 0 )
                return e;
        }
        return BSONElement();
    }

    /* add all the fields from the object specified to this object */
    inline BSONObjBuilder& BSONObjBuilder::appendElements(BSONObj x) {
        BSONObjIterator it(x);
        while ( it.moreWithEOO() ) {
            BSONElement e = it.next();
            if ( e.eoo() ) break;
            append(e);
        }
        return *this;
    }

    inline bool BSONObj::isValid(){
        int x = objsize();
        return x > 0 && x <= 1024 * 1024 * 8;
    }

    inline bool BSONObj::getObjectID(BSONElement& e) const { 
        BSONElement f = getField("_id");
        if( !f.eoo() ) { 
            e = f;
            return true;
        }
        return false;
    }

    inline BSONObjBuilderValueStream::BSONObjBuilderValueStream( BSONObjBuilder * builder ) {
        _fieldName = 0;
        _builder = builder;
    }
    
    template<class T> 
    inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<( T value ) { 
        _builder->append(_fieldName, value);
        _fieldName = 0;
        return *_builder;
    }

    inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<( const BSONElement& e ) { 
        _builder->appendAs( e , _fieldName );
        _fieldName = 0;
        return *_builder;
    }

    inline BSONObjBuilder& BSONObjBuilderValueStream::operator<<(DateNowLabeler& id){
        _builder->appendDate(_fieldName, jsTime());
        _fieldName = 0;
        return *_builder;
    }

    inline Labeler BSONObjBuilderValueStream::operator<<( const Labeler::Label &l ) { 
        return Labeler( l, this );
    }

    inline void BSONObjBuilderValueStream::endField( const char *nextFieldName ) {
        if ( _fieldName && haveSubobj() ) {
            _builder->append( _fieldName, subobj()->done() );
        }
        _subobj.reset();
        _fieldName = nextFieldName;
    }    

    inline BSONObjBuilder *BSONObjBuilderValueStream::subobj() {
        if ( !haveSubobj() )
            _subobj.reset( new BSONObjBuilder() );
        return _subobj.get();
    }
    
    template<class T> inline
    BSONObjBuilder& Labeler::operator<<( T value ) {
        s_->subobj()->append( l_.l_, value );
        return *s_->_builder;
    }    

    inline
    BSONObjBuilder& Labeler::operator<<( const BSONElement& e ) {
        s_->subobj()->appendAs( e, l_.l_ );
        return *s_->_builder;
    }    

    // {a: {b:1}} -> {a.b:1}
    void nested2dotted(BSONObjBuilder& b, const BSONObj& obj, const string& base="");
    inline BSONObj nested2dotted(const BSONObj& obj){
        BSONObjBuilder b;
        nested2dotted(b, obj);
        return b.obj();
    }

    // {a.b:1} -> {a: {b:1}}
    void dotted2nested(BSONObjBuilder& b, const BSONObj& obj);
    inline BSONObj dotted2nested(const BSONObj& obj){
        BSONObjBuilder b;
        dotted2nested(b, obj);
        return b.obj();
    }

    inline BSONObjIterator BSONObjBuilder::iterator() const {
        const char * s = b.buf() + offset_;
        const char * e = b.buf() + b.len();
        return BSONObjIterator( s , e );
    }
    
    /* WARNING: nested/dotted conversions are not 100% reversible
     * nested2dotted(dotted2nested({a.b: {c:1}})) -> {a.b.c: 1}
     * also, dotted2nested ignores order
     */

    typedef map<string, BSONElement> BSONMap;
    inline BSONMap bson2map(const BSONObj& obj){
        BSONMap m;
        BSONObjIterator it(obj);
        while (it.more()){
            BSONElement e = it.next();
            m[e.fieldName()] = e;
        }
        return m;
    }

    struct BSONElementFieldNameCmp {
        bool operator()( const BSONElement &l, const BSONElement &r ) const {
            return strcmp( l.fieldName() , r.fieldName() ) <= 0;
        }
    };

    typedef set<BSONElement, BSONElementFieldNameCmp> BSONSortedElements;
    inline BSONSortedElements bson2set( const BSONObj& obj ){
        BSONSortedElements s;
        BSONObjIterator it(obj);
        while ( it.more() )
            s.insert( it.next() );
        return s;
    }

    /** A precomputation of a BSON key pattern.
        The constructor is private to make conversion more explicit so we notice where we call make().
        Over time we should push this up higher and higher.
        */
    class Ordering { 
        const unsigned bits;
        const unsigned nkeys;
        Ordering(unsigned b,unsigned n) : bits(b),nkeys(n) { }
    public:
        /** so, for key pattern { a : 1, b : -1 }
            get(0) == 1
            get(1) == -1
        */
        int get(int i) const { 
            return ((1 << i) & bits) ? -1 : 1;
        }

        // for woCompare...
        unsigned descending(unsigned mask) const { return bits & mask; }
        
        operator string() const {
            StringBuilder buf(32);
            for ( unsigned i=0; i<nkeys; i++)
                buf.append( get(i) > 0 ? "+" : "-" );
            return buf.str();
        }

        static Ordering make(const BSONObj& obj) {
            unsigned b = 0;
            BSONObjIterator k(obj);
            unsigned n = 0;
            while( 1 ) { 
                BSONElement e = k.next();
                if( e.eoo() )
                    break;
                uassert( 13103, "too many compound keys", n <= 31 );
                if( e.number() < 0 )
                    b |= (1 << n);
                n++;
            }
            return Ordering(b,n);
        }
    };
    
    class BSONObjIteratorSorted {
    public:
        BSONObjIteratorSorted( const BSONObj& o );
        
        ~BSONObjIteratorSorted(){
            assert( _fields );
            delete[] _fields;
            _fields = 0;
        }

        bool more(){
            return _cur < _nfields;
        }
        
        BSONElement next(){
            assert( _fields );
            if ( _cur < _nfields )
                return BSONElement( _fields[_cur++] );
            return BSONElement();
        }

    private:
        const char ** _fields;
        int _nfields;
        int _cur;
    };

} // namespace mongo
