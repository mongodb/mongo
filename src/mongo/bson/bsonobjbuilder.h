/* bsonobjbuilder.h

   Classes in this file:
   BSONObjBuilder
   BSONArrayBuilder
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

#pragma once

#include <limits>
#include <cmath>
#include <boost/static_assert.hpp>
#if defined(MONGO_EXPOSE_MACROS)
#define verify MONGO_verify
#endif
#include "bsonelement.h"
#include "bsonobj.h"
#include "bsonmisc.h"
#if defined(_DEBUG) && defined(MONGO_EXPOSE_MACROS)
#include "../util/log.h"
#endif

namespace mongo {

#if defined(_WIN32)
// warning: 'this' : used in base member initializer list
#pragma warning( disable : 4355 )
#endif

    template<typename T>
    class BSONFieldValue {
    public:
        BSONFieldValue( const std::string& name , const T& t ) {
            _name = name;
            _t = t;
        }

        const T& value() const { return _t; }
        const std::string& name() const { return _name; }

    private:
        std::string _name;
        T _t;
    };

    template<typename T>
    class BSONField {
    public:
        BSONField( const std::string& name , const std::string& longName="" )
            : _name(name), _longName(longName) {}
        const std::string& name() const { return _name; }
        operator std::string() const { return _name; }

        BSONFieldValue<T> make( const T& t ) const {
            return BSONFieldValue<T>( _name , t );
        }

        BSONFieldValue<BSONObj> gt( const T& t ) const { return query( "$gt" , t ); }
        BSONFieldValue<BSONObj> lt( const T& t ) const { return query( "$lt" , t ); }

        BSONFieldValue<BSONObj> query( const char * q , const T& t ) const;

        BSONFieldValue<T> operator()( const T& t ) const {
            return BSONFieldValue<T>( _name , t );
        }

    private:
        std::string _name;
        std::string _longName;
    };

    /** Utility for creating a BSONObj.
        See also the BSON() and BSON_ARRAY() macros.
    */
    class BSONObjBuilder : boost::noncopyable {
    public:
        /** @param initsize this is just a hint as to the final size of the object */
        BSONObjBuilder(int initsize=512) : _b(_buf), _buf(initsize + sizeof(unsigned)), _offset( sizeof(unsigned) ), _s( this ) , _tracker(0) , _doneCalled(false) {
            _b.appendNum((unsigned)0); // ref-count
            _b.skip(4); /*leave room for size field and ref-count*/
        }

        /** @param baseBuilder construct a BSONObjBuilder using an existing BufBuilder
         *  This is for more efficient adding of subobjects/arrays. See docs for subobjStart for example.
         */
        BSONObjBuilder( BufBuilder &baseBuilder ) : _b( baseBuilder ), _buf( 0 ), _offset( baseBuilder.len() ), _s( this ) , _tracker(0) , _doneCalled(false) {
            _b.skip( 4 );
        }

        BSONObjBuilder( const BSONSizeTracker & tracker ) : _b(_buf) , _buf(tracker.getSize() + sizeof(unsigned) ), _offset( sizeof(unsigned) ), _s( this ) , _tracker( (BSONSizeTracker*)(&tracker) ) , _doneCalled(false) {
            _b.appendNum((unsigned)0); // ref-count
            _b.skip(4);
        }

        ~BSONObjBuilder() {
            if ( !_doneCalled && _b.buf() && _buf.getSize() == 0 ) {
                _done();
            }
        }

        /** add all the fields from the object specified to this object */
        BSONObjBuilder& appendElements(BSONObj x);

        /** add all the fields from the object specified to this object if they don't exist already */
        BSONObjBuilder& appendElementsUnique( BSONObj x );

        /** append element to the object we are building */
        BSONObjBuilder& append( const BSONElement& e) {
            verify( !e.eoo() ); // do not append eoo, that would corrupt us. the builder auto appends when done() is called.
            _b.appendBuf((void*) e.rawdata(), e.size());
            return *this;
        }

        /** append an element but with a new name */
        BSONObjBuilder& appendAs(const BSONElement& e, const StringData& fieldName) {
            verify( !e.eoo() ); // do not append eoo, that would corrupt us. the builder auto appends when done() is called.
            _b.appendNum((char) e.type());
            _b.appendStr(fieldName);
            _b.appendBuf((void *) e.value(), e.valuesize());
            return *this;
        }

        /** add a subobject as a member */
        BSONObjBuilder& append(const StringData& fieldName, BSONObj subObj) {
            _b.appendNum((char) Object);
            _b.appendStr(fieldName);
            _b.appendBuf((void *) subObj.objdata(), subObj.objsize());
            return *this;
        }

        /** add a subobject as a member */
        BSONObjBuilder& appendObject(const StringData& fieldName, const char * objdata , int size = 0 ) {
            verify( objdata );
            if ( size == 0 ) {
                size = little<int>::ref( objdata );
            }

            verify( size > 4 && size < 100000000 );

            _b.appendNum((char) Object);
            _b.appendStr(fieldName);
            _b.appendBuf((void*)objdata, size );
            return *this;
        }

        /** add header for a new subobject and return bufbuilder for writing to
         *  the subobject's body
         *
         *  example:
         *
         *  BSONObjBuilder b;
         *  BSONObjBuilder sub (b.subobjStart("fieldName"));
         *  // use sub
         *  sub.done()
         *  // use b and convert to object
         */
        BufBuilder &subobjStart(const StringData& fieldName) {
            _b.appendNum((char) Object);
            _b.appendStr(fieldName);
            return _b;
        }

        /** add a subobject as a member with type Array.  Thus arr object should have "0", "1", ...
            style fields in it.
        */
        BSONObjBuilder& appendArray(const StringData& fieldName, const BSONObj &subObj) {
            _b.appendNum((char) Array);
            _b.appendStr(fieldName);
            _b.appendBuf((void *) subObj.objdata(), subObj.objsize());
            return *this;
        }
        BSONObjBuilder& append(const StringData& fieldName, BSONArray arr) {
            return appendArray(fieldName, arr);
        }

        /** add header for a new subarray and return bufbuilder for writing to
            the subarray's body */
        BufBuilder &subarrayStart(const StringData& fieldName) {
            _b.appendNum((char) Array);
            _b.appendStr(fieldName);
            return _b;
        }

        /** Append a boolean element */
        BSONObjBuilder& appendBool(const StringData& fieldName, int val) {
            _b.appendNum((char) Bool);
            _b.appendStr(fieldName);
            _b.appendNum((char) (val?1:0));
            return *this;
        }

        /** Append a boolean element */
        BSONObjBuilder& append(const StringData& fieldName, bool val) {
            _b.appendNum((char) Bool);
            _b.appendStr(fieldName);
            _b.appendNum((char) (val?1:0));
            return *this;
        }

        /** Append a 32 bit integer element */
        BSONObjBuilder& append(const StringData& fieldName, int n) {
            _b.appendNum((char) NumberInt);
            _b.appendStr(fieldName);
            _b.appendNum(n);
            return *this;
        }

        /** Append a 32 bit unsigned element - cast to a signed int. */
        BSONObjBuilder& append(const StringData& fieldName, unsigned n) {
            return append(fieldName, (int) n);
        }

        /** Append a NumberLong */
        BSONObjBuilder& append(const StringData& fieldName, long long n) {
            _b.appendNum((char) NumberLong);
            _b.appendStr(fieldName);
            _b.appendNum(n);
            return *this;
        }

        /** appends a number.  if n < max(int)/2 then uses int, otherwise long long */
        BSONObjBuilder& appendIntOrLL( const StringData& fieldName , long long n ) {
            long long x = n;
            if ( x < 0 )
                x = x * -1;
            if ( x < ( (std::numeric_limits<int>::max)() / 2 ) ) // extra () to avoid max macro on windows
                append( fieldName , (int)n );
            else
                append( fieldName , n );
            return *this;
        }

        /**
         * appendNumber is a series of method for appending the smallest sensible type
         * mostly for JS
         */
        BSONObjBuilder& appendNumber( const StringData& fieldName , int n ) {
            return append( fieldName , n );
        }

        BSONObjBuilder& appendNumber( const StringData& fieldName , double d ) {
            return append( fieldName , d );
        }

        BSONObjBuilder& appendNumber( const StringData& fieldName , size_t n ) {
            static size_t maxInt = (size_t)pow( 2.0 , 30.0 );

            if ( n < maxInt )
                append( fieldName , (int)n );
            else
                append( fieldName , (long long)n );
            return *this;
        }

        BSONObjBuilder& appendNumber( const StringData& fieldName , long long l ) {
            static long long maxInt = (int)pow( 2.0 , 30.0 );
            static long long maxDouble = (long long)pow( 2.0 , 40.0 );
            long long x = l >= 0 ? l : -l;
            if ( x < maxInt )
                append( fieldName , (int)l );
            else if ( x < maxDouble )
                append( fieldName , (double)l );
            else
                append( fieldName , l );
            return *this;
        }

        /** Append a double element */
        BSONObjBuilder& append(const StringData& fieldName, double n) {
            _b.appendNum((char) NumberDouble);
            _b.appendStr(fieldName);
            _b.appendNum(n);
            return *this;
        }

        /** tries to append the data as a number
         * @return true if the data was able to be converted to a number
         */
        bool appendAsNumber( const StringData& fieldName , const std::string& data );

        /** Append a BSON Object ID (OID type).
            @deprecated Generally, it is preferred to use the append append(name, oid)
            method for this.
        */
        BSONObjBuilder& appendOID(const StringData& fieldName, OID *oid = 0 , bool generateIfBlank = false ) {
            _b.appendNum((char) jstOID);
            _b.appendStr(fieldName);
            if ( oid )
                _b.appendBuf( (void *) oid, 12 );
            else {
                OID tmp;
                if ( generateIfBlank )
                    tmp.init();
                else
                    tmp.clear();
                _b.appendBuf( (void *) &tmp, 12 );
            }
            return *this;
        }

        /**
        Append a BSON Object ID.
        @param fieldName Field name, e.g., "_id".
        @returns the builder object
        */
        BSONObjBuilder& append( const StringData& fieldName, OID oid ) {
            _b.appendNum((char) jstOID);
            _b.appendStr(fieldName);
            _b.appendBuf( (void *) &oid, 12 );
            return *this;
        }

        /**
        Generate and assign an object id for the _id field.
        _id should be the first element in the object for good performance.
        */
        BSONObjBuilder& genOID() {
            return append("_id", OID::gen());
        }

        /** Append a time_t date.
            @param dt a C-style 32 bit date value, that is
            the number of seconds since January 1, 1970, 00:00:00 GMT
        */
        BSONObjBuilder& appendTimeT(const StringData& fieldName, time_t dt) {
            _b.appendNum((char) Date);
            _b.appendStr(fieldName);
            _b.appendNum(static_cast<unsigned long long>(dt) * 1000);
            return *this;
        }
        /** Append a date.
            @param dt a Java-style 64 bit date value, that is
            the number of milliseconds since January 1, 1970, 00:00:00 GMT
        */
        BSONObjBuilder& appendDate(const StringData& fieldName, Date_t dt) {
            /* easy to pass a time_t to this and get a bad result.  thus this warning. */
#if defined(_DEBUG) && defined(MONGO_EXPOSE_MACROS)
            if( dt > 0 && dt <= 0xffffffff ) {
                static int n;
                if( n++ == 0 )
                    log() << "DEV WARNING appendDate() called with a tiny (but nonzero) date" << std::endl;
            }
#endif
            _b.appendNum((char) Date);
            _b.appendStr(fieldName);
            _b.appendNum(dt);
            return *this;
        }
        BSONObjBuilder& append(const StringData& fieldName, Date_t dt) {
            return appendDate(fieldName, dt);
        }

        /** Append a regular expression value
            @param regex the regular expression pattern
            @param regex options such as "i" or "g"
        */
        BSONObjBuilder& appendRegex(const StringData& fieldName, const StringData& regex, const StringData& options = "") {
            _b.appendNum((char) RegEx);
            _b.appendStr(fieldName);
            _b.appendStr(regex);
            _b.appendStr(options);
            return *this;
        }

        BSONObjBuilder& appendCode(const StringData& fieldName, const StringData& code) {
            _b.appendNum((char) Code);
            _b.appendStr(fieldName);
            _b.appendNum((int) code.size()+1);
            _b.appendStr(code);
            return *this;
        }

        /** Append a string element. 
            @param sz size includes terminating null character */
        BSONObjBuilder& append(const StringData& fieldName, const char *str, int sz) {
            _b.appendNum((char) String);
            _b.appendStr(fieldName);
            _b.appendNum((int)sz);
            _b.appendBuf(str, sz);
            return *this;
        }
        /** Append a string element */
        BSONObjBuilder& append(const StringData& fieldName, const char *str) {
            return append(fieldName, str, (int) strlen(str)+1);
        }
        /** Append a string element */
        BSONObjBuilder& append(const StringData& fieldName, const std::string& str) {
            return append(fieldName, str.c_str(), (int) str.size()+1);
        }

        BSONObjBuilder& appendSymbol(const StringData& fieldName, const StringData& symbol) {
            _b.appendNum((char) Symbol);
            _b.appendStr(fieldName);
            _b.appendNum((int) symbol.size()+1);
            _b.appendStr(symbol);
            return *this;
        }

        /** Append a Null element to the object */
        BSONObjBuilder& appendNull( const StringData& fieldName ) {
            _b.appendNum( (char) jstNULL );
            _b.appendStr( fieldName );
            return *this;
        }

        // Append an element that is less than all other keys.
        BSONObjBuilder& appendMinKey( const StringData& fieldName ) {
            _b.appendNum( (char) MinKey );
            _b.appendStr( fieldName );
            return *this;
        }
        // Append an element that is greater than all other keys.
        BSONObjBuilder& appendMaxKey( const StringData& fieldName ) {
            _b.appendNum( (char) MaxKey );
            _b.appendStr( fieldName );
            return *this;
        }

        // Append a Timestamp field -- will be updated to next OpTime on db insert.
        BSONObjBuilder& appendTimestamp( const StringData& fieldName ) {
            _b.appendNum( (char) Timestamp );
            _b.appendStr( fieldName );
            _b.appendNum( (unsigned long long) 0 );
            return *this;
        }

        BSONObjBuilder& appendTimestamp( const StringData& fieldName , unsigned long long val ) {
            _b.appendNum( (char) Timestamp );
            _b.appendStr( fieldName );
            _b.appendNum( val );
            return *this;
        }

        /**
        Timestamps are a special BSON datatype that is used internally for replication.
        Append a timestamp element to the object being ebuilt.
        @param time - in millis (but stored in seconds)
        */
        BSONObjBuilder& appendTimestamp( const StringData& fieldName , unsigned long long time , unsigned int inc );

        /*
        Append an element of the deprecated DBRef type.
        @deprecated
        */
        BSONObjBuilder& appendDBRef( const StringData& fieldName, const StringData& ns, const OID &oid ) {
            _b.appendNum( (char) DBRef );
            _b.appendStr( fieldName );
            _b.appendNum( (int) ns.size() + 1 );
            _b.appendStr( ns );
            _b.appendBuf( (void *) &oid, 12 );
            return *this;
        }

        /** Append a binary data element
            @param fieldName name of the field
            @param len length of the binary data in bytes
            @param subtype subtype information for the data. @see enum BinDataType in bsontypes.h.
                   Use BinDataGeneral if you don't care about the type.
            @param data the byte array
        */
        BSONObjBuilder& appendBinData( const StringData& fieldName, int len, BinDataType type, const void *data ) {
            _b.appendNum( (char) BinData );
            _b.appendStr( fieldName );
            _b.appendNum( len );
            _b.appendNum( (char) type );
            _b.appendBuf( data, len );
            return *this;
        }

        /**
        Subtype 2 is deprecated.
        Append a BSON bindata bytearray element.
        @param data a byte array
        @param len the length of data
        */
        BSONObjBuilder& appendBinDataArrayDeprecated( const char * fieldName , const void * data , int len ) {
            _b.appendNum( (char) BinData );
            _b.appendStr( fieldName );
            _b.appendNum( len + 4 );
            _b.appendNum( (char)0x2 );
            _b.appendNum( len );
            _b.appendBuf( data, len );
            return *this;
        }

        /** Append to the BSON object a field of type CodeWScope.  This is a javascript code
            fragment accompanied by some scope that goes with it.
        */
        BSONObjBuilder& appendCodeWScope( const StringData& fieldName, const StringData& code, const BSONObj &scope ) {
            _b.appendNum( (char) CodeWScope );
            _b.appendStr( fieldName );
            _b.appendNum( ( int )( 4 + 4 + code.size() + 1 + scope.objsize() ) );
            _b.appendNum( ( int ) code.size() + 1 );
            _b.appendStr( code );
            _b.appendBuf( ( void * )scope.objdata(), scope.objsize() );
            return *this;
        }

        void appendUndefined( const StringData& fieldName ) {
            _b.appendNum( (char) Undefined );
            _b.appendStr( fieldName );
        }

        /* helper function -- see Query::where() for primary way to do this. */
        void appendWhere( const StringData& code, const BSONObj &scope ) {
            appendCodeWScope( "$where" , code , scope );
        }

        /**
           these are the min/max when comparing, not strict min/max elements for a given type
        */
        void appendMinForType( const StringData& fieldName , int type );
        void appendMaxForType( const StringData& fieldName , int type );

        /** Append an array of values. */
        template < class T >
        BSONObjBuilder& append( const StringData& fieldName, const std::vector< T >& vals );

        template < class T >
        BSONObjBuilder& append( const StringData& fieldName, const std::list< T >& vals );

        /** Append a set of values. */
        template < class T >
        BSONObjBuilder& append( const StringData& fieldName, const std::set< T >& vals );

        /**
         * destructive
         * The returned BSONObj will free the buffer when it is finished.
         * @return owned BSONObj
        */
        BSONObj obj() {
            bool own = owned();
            massert( 10335 , "builder does not own memory", own );
            doneFast();
            BSONObj::Holder* h = (BSONObj::Holder*)_b.buf();
            decouple(); // sets _b.buf() to NULL
            return BSONObj(h);
        }

        /** Fetch the object we have built.
            BSONObjBuilder still frees the object when the builder goes out of
            scope -- very important to keep in mind.  Use obj() if you
            would like the BSONObj to last longer than the builder.
        */
        BSONObj done() {
            return BSONObj(_done());
        }

        // Like 'done' above, but does not construct a BSONObj to return to the caller.
        void doneFast() {
            (void)_done();
        }

        /** Peek at what is in the builder, but leave the builder ready for more appends.
            The returned object is only valid until the next modification or destruction of the builder.
            Intended use case: append a field if not already there.
        */
        BSONObj asTempObj() {
            BSONObj temp(_done());
            _b.setlen(_b.len()-1); //next append should overwrite the EOO
            _doneCalled = false;
            return temp;
        }

        /* assume ownership of the buffer - you must then free it (with free()) */
        char* decouple(int& l) {
            char *x = _done();
            verify( x );
            l = _b.len();
            _b.decouple();
            return x;
        }
        void decouple() {
            _b.decouple();    // post done() call version.  be sure jsobj frees...
        }

        void appendKeys( const BSONObj& keyPattern , const BSONObj& values );

        static std::string numStr( int i ) {
            if (i>=0 && i<100 && numStrsReady)
                return numStrs[i];
            StringBuilder o;
            o << i;
            return o.str();
        }

        /** Stream oriented way to add field names and values. */
        BSONObjBuilderValueStream &operator<<(const char * name ) {
            _s.endField( name );
            return _s;
        }

        /** Stream oriented way to add field names and values. */
        BSONObjBuilder& operator<<( GENOIDLabeler ) { return genOID(); }

        // prevent implicit string conversions which would allow bad things like BSON( BSON( "foo" << 1 ) << 2 )
        struct ForceExplicitString {
            ForceExplicitString( const std::string &str ) : str_( str ) {}
            std::string str_;
        };

        /** Stream oriented way to add field names and values. */
        BSONObjBuilderValueStream &operator<<( const ForceExplicitString& name ) {
            return operator<<( name.str_.c_str() );
        }

        Labeler operator<<( const Labeler::Label &l ) {
            massert( 10336 ,  "No subobject started", _s.subobjStarted() );
            return _s << l;
        }

        template<typename T>
        BSONObjBuilderValueStream& operator<<( const BSONField<T>& f ) {
            _s.endField( f.name().c_str() );
            return _s;
        }

        template<typename T>
        BSONObjBuilder& operator<<( const BSONFieldValue<T>& v ) {
            append( v.name().c_str() , v.value() );
            return *this;
        }

        BSONObjBuilder& operator<<( const BSONElement& e ){
            append( e );
            return *this;
        }

        /** @return true if we are using our own bufbuilder, and not an alternate that was given to us in our constructor */
        bool owned() const { return &_b == &_buf; }

        BSONObjIterator iterator() const ;

        bool hasField( const StringData& name ) const ;

        int len() const { return _b.len(); }

        BufBuilder& bb() { return _b; }

    private:
        char* _done() {
            if ( _doneCalled )
                return _b.buf() + _offset;

            _doneCalled = true;
            _s.endField();
            _b.appendNum((char) EOO);
            char *data = _b.buf() + _offset;
            int size = _b.len() - _offset;
            little<int>::ref( data ) = size;
            if ( _tracker )
                _tracker->got( size );
            return data;
        }

        BufBuilder &_b;
        BufBuilder _buf;
        int _offset;
        BSONObjBuilderValueStream _s;
        BSONSizeTracker * _tracker;
        bool _doneCalled;

        static const std::string numStrs[100]; // cache of 0 to 99 inclusive
        static bool numStrsReady; // for static init safety. see comments in db/jsobj.cpp
    };

    class BSONArrayBuilder : boost::noncopyable {
    public:
        BSONArrayBuilder() : _i(0), _b() {}
        BSONArrayBuilder( BufBuilder &_b ) : _i(0), _b(_b) {}
        BSONArrayBuilder( int initialSize ) : _i(0), _b(initialSize) {}

        template <typename T>
        BSONArrayBuilder& append(const T& x) {
            _b.append(num(), x);
            return *this;
        }

        BSONArrayBuilder& append(const BSONElement& e) {
            _b.appendAs(e, num());
            return *this;
        }

        template <typename T>
        BSONArrayBuilder& operator<<(const T& x) {
            return append(x);
        }

        void appendNull() {
            _b.appendNull(num());
        }

        /**
         * destructive - ownership moves to returned BSONArray
         * @return owned BSONArray
         */
        BSONArray arr() { return BSONArray(_b.obj()); }

        BSONObj done() { return _b.done(); }

        void doneFast() { _b.doneFast(); }

        template <typename T>
        BSONArrayBuilder& append(const StringData& name, const T& x) {
            fill( name );
            append( x );
            return *this;
        }

        template < class T >
        BSONArrayBuilder& append( const std::list< T >& vals );

        template < class T >
        BSONArrayBuilder& append( const std::set< T >& vals );

        // These two just use next position
        BufBuilder &subobjStart() { return _b.subobjStart( num() ); }
        BufBuilder &subarrayStart() { return _b.subarrayStart( num() ); }

        // These fill missing entries up to pos. if pos is < next pos is ignored
        BufBuilder &subobjStart(int pos) {
            fill(pos);
            return _b.subobjStart( num() );
        }
        BufBuilder &subarrayStart(int pos) {
            fill(pos);
            return _b.subarrayStart( num() );
        }

        // These should only be used where you really need interface compatability with BSONObjBuilder
        // Currently they are only used by update.cpp and it should probably stay that way
        BufBuilder &subobjStart( const StringData& name ) {
            fill( name );
            return _b.subobjStart( num() );
        }

        BufBuilder &subarrayStart( const char *name ) {
            fill( name );
            return _b.subarrayStart( num() );
        }

        void appendArray( const StringData& name, BSONObj subObj ) {
            fill( name );
            _b.appendArray( num(), subObj );
        }

        void appendAs( const BSONElement &e, const char *name) {
            fill( name );
            append( e );
        }

        int len() const { return _b.len(); }
        int arrSize() const { return _i; }

    private:
        // These two are undefined privates to prevent their accidental
        // use as we don't support unsigned ints in BSON
        BSONObjBuilder& append(const StringData& fieldName, unsigned int val);
        BSONObjBuilder& append(const StringData& fieldName, unsigned long long val);

        void fill( const StringData& name ) {
            char *r;
            long int n = strtol( name.data(), &r, 10 );
            if ( *r )
                uasserted( 13048, (std::string)"can't append to array using string field name [" + name.data() + "]" );
            fill(n);
        }

        void fill (int upTo){
            // if this is changed make sure to update error message and jstests/set7.js
            const int maxElems = 1500000;
            BOOST_STATIC_ASSERT(maxElems < (BSONObjMaxUserSize/10));
            uassert(15891, "can't backfill array to larger than 1,500,000 elements", upTo <= maxElems);

            while( _i < upTo )
                append( nullElt() );
        }

        static BSONElement nullElt() {
            static BSONObj n = nullObj();
            return n.firstElement();
        }

        static BSONObj nullObj() {
            BSONObjBuilder _b;
            _b.appendNull( "" );
            return _b.obj();
        }

        std::string num() { return _b.numStr(_i++); }
        int _i;
        BSONObjBuilder _b;
    };

    template < class T >
    inline BSONObjBuilder& BSONObjBuilder::append( const StringData& fieldName, const std::vector< T >& vals ) {
        BSONObjBuilder arrBuilder;
        for ( unsigned int i = 0; i < vals.size(); ++i )
            arrBuilder.append( numStr( i ), vals[ i ] );
        appendArray( fieldName, arrBuilder.done() );
        return *this;
    }

    template < class L >
    inline BSONObjBuilder& _appendIt( BSONObjBuilder& _this, const StringData& fieldName, const L& vals ) {
        BSONObjBuilder arrBuilder;
        int n = 0;
        for( typename L::const_iterator i = vals.begin(); i != vals.end(); i++ )
            arrBuilder.append( BSONObjBuilder::numStr(n++), *i );
        _this.appendArray( fieldName, arrBuilder.done() );
        return _this;
    }

    template < class T >
    inline BSONObjBuilder& BSONObjBuilder::append( const StringData& fieldName, const std::list< T >& vals ) {
        return _appendIt< std::list< T > >( *this, fieldName, vals );
    }

    template < class T >
    inline BSONObjBuilder& BSONObjBuilder::append( const StringData& fieldName, const std::set< T >& vals ) {
        return _appendIt< std::set< T > >( *this, fieldName, vals );
    }

    template < class L >
    inline BSONArrayBuilder& _appendArrayIt( BSONArrayBuilder& _this, const L& vals ) {
        for( typename L::const_iterator i = vals.begin(); i != vals.end(); i++ )
            _this.append( *i );
        return _this;
    }

    template < class T >
    inline BSONArrayBuilder& BSONArrayBuilder::append( const std::list< T >& vals ) {
        return _appendArrayIt< std::list< T > >( *this, vals );
    }

    template < class T >
    inline BSONArrayBuilder& BSONArrayBuilder::append( const std::set< T >& vals ) {
        return _appendArrayIt< std::set< T > >( *this, vals );
    }


    // $or helper: OR(BSON("x" << GT << 7), BSON("y" << LT 6));
    inline BSONObj OR(const BSONObj& a, const BSONObj& b)
    { return BSON( "$or" << BSON_ARRAY(a << b) ); }
    inline BSONObj OR(const BSONObj& a, const BSONObj& b, const BSONObj& c)
    { return BSON( "$or" << BSON_ARRAY(a << b << c) ); }
    inline BSONObj OR(const BSONObj& a, const BSONObj& b, const BSONObj& c, const BSONObj& d)
    { return BSON( "$or" << BSON_ARRAY(a << b << c << d) ); }
    inline BSONObj OR(const BSONObj& a, const BSONObj& b, const BSONObj& c, const BSONObj& d, const BSONObj& e)
    { return BSON( "$or" << BSON_ARRAY(a << b << c << d << e) ); }
    inline BSONObj OR(const BSONObj& a, const BSONObj& b, const BSONObj& c, const BSONObj& d, const BSONObj& e, const BSONObj& f)
    { return BSON( "$or" << BSON_ARRAY(a << b << c << d << e << f) ); }

}
