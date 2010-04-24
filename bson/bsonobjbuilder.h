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

namespace mongo {

    /** Utility for creating a BSONObj.
        See also the BSON() and BSON_ARRAY() macros.
    */
    class BSONObjBuilder : boost::noncopyable {
    public:
        /** @param initsize this is just a hint as to the final size of the object */
        BSONObjBuilder(int initsize=512) : b(buf_), buf_(initsize), offset_( 0 ), s_( this ) , _tracker(0) {
            b.skip(4); /*leave room for size field*/
        }

        /** @param baseBuilder construct a BSONObjBuilder using an existing BufBuilder */
        BSONObjBuilder( BufBuilder &baseBuilder ) : b( baseBuilder ), buf_( 0 ), offset_( baseBuilder.len() ), s_( this ) , _tracker(0) {
            b.skip( 4 );
        }
        
        BSONObjBuilder( const BSONSizeTracker & tracker ) : b(buf_) , buf_(tracker.getSize() ), offset_(0), s_( this ) , _tracker( (BSONSizeTracker*)(&tracker) ){
            b.skip( 4 );
        }

        /** add all the fields from the object specified to this object */
        BSONObjBuilder& appendElements(BSONObj x);

        /** append element to the object we are building */
        BSONObjBuilder& append( const BSONElement& e) {
            assert( !e.eoo() ); // do not append eoo, that would corrupt us. the builder auto appends when done() is called.
            b.append((void*) e.rawdata(), e.size());
            return *this;
        }

        /** append an element but with a new name */
        BSONObjBuilder&  appendAs(const BSONElement& e, const char *as) {
            assert( !e.eoo() ); // do not append eoo, that would corrupt us. the builder auto appends when done() is called.
            b.append((char) e.type());
            b.append(as);
            b.append((void *) e.value(), e.valuesize());
            return *this;
        }

        /** append an element but with a new name */
        BSONObjBuilder& appendAs(const BSONElement& e, const string& as) {
            appendAs( e , as.c_str() );
            return *this;
        }

        /** add a subobject as a member */
        BSONObjBuilder& append(const char *fieldName, BSONObj subObj) {
            b.append((char) Object);
            b.append(fieldName);
            b.append((void *) subObj.objdata(), subObj.objsize());
            return *this;
        }

        /** add a subobject as a member */
        BSONObjBuilder& append(const string& fieldName , BSONObj subObj) {
            return append( fieldName.c_str() , subObj );
        }

        /** add header for a new subobject and return bufbuilder for writing to
            the subobject's body */
        BufBuilder &subobjStart(const char *fieldName) {
            b.append((char) Object);
            b.append(fieldName);
            return b;
        }
        
        /** add a subobject as a member with type Array.  Thus arr object should have "0", "1", ...
            style fields in it.
        */
        BSONObjBuilder& appendArray(const char *fieldName, BSONObj subObj) {
            b.append((char) Array);
            b.append(fieldName);
            b.append((void *) subObj.objdata(), subObj.objsize());
            return *this;
        }
        BSONObjBuilder& append(const char *fieldName, BSONArray arr) { 
            return appendArray(fieldName, arr); 
        }    

        /** add header for a new subarray and return bufbuilder for writing to
            the subarray's body */
        BufBuilder &subarrayStart(const char *fieldName) {
            b.append((char) Array);
            b.append(fieldName);
            return b;
        }
        
        /** Append a boolean element */
        BSONObjBuilder& appendBool(const char *fieldName, int val) {
            b.append((char) Bool);
            b.append(fieldName);
            b.append((char) (val?1:0));
            return *this;
        }

        /** Append a boolean element */
        BSONObjBuilder& append(const char *fieldName, bool val) {
            b.append((char) Bool);
            b.append(fieldName);
            b.append((char) (val?1:0));            
            return *this;
        }
        
        /** Append a 32 bit integer element */
        BSONObjBuilder& append(const char *fieldName, int n) {
            b.append((char) NumberInt);
            b.append(fieldName);
            b.append(n);
            return *this;
        }
        /** Append a 32 bit integer element */
        BSONObjBuilder& append(const string &fieldName, int n) {
            return append( fieldName.c_str(), n );
        }

        /** Append a 32 bit unsigned element - cast to a signed int. */
        BSONObjBuilder& append(const char *fieldName, unsigned n) { 
            return append(fieldName, (int) n); 
        }

        /** Append a NumberLong */
        BSONObjBuilder& append(const char *fieldName, long long n) { 
            b.append((char) NumberLong);
            b.append(fieldName);
            b.append(n);
            return *this; 
        }

        /** Append a NumberLong */
        BSONObjBuilder& append(const string& fieldName, long long n) { 
            return append( fieldName.c_str() , n );
        }

        /** appends a number.  if n < max(int)/2 then uses int, otherwise long long */
        BSONObjBuilder& appendIntOrLL( const string& fieldName , long long n ){
            long long x = n;
            if ( x < 0 )
                x = x * -1;
            if ( x < ( numeric_limits<int>::max() / 2 ) )
                append( fieldName.c_str() , (int)n );
            else
                append( fieldName.c_str() , n );
            return *this;
        }

        /**
         * appendNumber is a series of method for appending the smallest sensible type
         * mostly for JS
         */
        BSONObjBuilder& appendNumber( const string& fieldName , int n ){
            return append( fieldName.c_str() , n );
        }

        BSONObjBuilder& appendNumber( const string& fieldName , double d ){
            return append( fieldName.c_str() , d );
        }

        BSONObjBuilder& appendNumber( const string& fieldName , long long l ){
            static long long maxInt = (int)pow( 2.0 , 30.0 );
            static long long maxDouble = (long long)pow( 2.0 , 40.0 );

            if ( l < maxInt )
                append( fieldName.c_str() , (int)l );
            else if ( l < maxDouble )
                append( fieldName.c_str() , (double)l );
            else
                append( fieldName.c_str() , l );
            return *this;
        }
        
        /** Append a double element */
        BSONObjBuilder& append(const char *fieldName, double n) {
            b.append((char) NumberDouble);
            b.append(fieldName);
            b.append(n);
            return *this;
        }

        /** tries to append the data as a number
         * @return true if the data was able to be converted to a number
         */
        bool appendAsNumber( const string& fieldName , const string& data );

        /** Append a BSON Object ID (OID type). 
            @deprecated Generally, it is preferred to use the append append(name, oid) 
            method for this.
        */
        BSONObjBuilder& appendOID(const char *fieldName, OID *oid = 0 , bool generateIfBlank = false ) {
            b.append((char) jstOID);
            b.append(fieldName);
            if ( oid )
                b.append( (void *) oid, 12 );
            else {
                OID tmp;
                if ( generateIfBlank )
                    tmp.init();
                else
                    tmp.clear();
                b.append( (void *) &tmp, 12 );
            }
            return *this;
        }

        /** 
        Append a BSON Object ID. 
        @param fieldName Field name, e.g., "_id".
        @returns the builder object
        */
        BSONObjBuilder& append( const char *fieldName, OID oid ) {
            b.append((char) jstOID);
            b.append(fieldName);
            b.append( (void *) &oid, 12 );
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
        BSONObjBuilder& appendTimeT(const char *fieldName, time_t dt) {
            b.append((char) Date);
            b.append(fieldName);
            b.append(static_cast<unsigned long long>(dt) * 1000);
            return *this;
        }
        /** Append a date.  
            @param dt a Java-style 64 bit date value, that is 
            the number of milliseconds since January 1, 1970, 00:00:00 GMT
        */
        BSONObjBuilder& appendDate(const char *fieldName, Date_t dt) {
            b.append((char) Date);
            b.append(fieldName);
            b.append(dt);
            return *this;
        }
        BSONObjBuilder& append(const char *fieldName, Date_t dt) {
            return appendDate(fieldName, dt);
        }

        /** Append a regular expression value
            @param regex the regular expression pattern
            @param regex options such as "i" or "g"
        */
        BSONObjBuilder& appendRegex(const char *fieldName, const char *regex, const char *options = "") {
            b.append((char) RegEx);
            b.append(fieldName);
            b.append(regex);
            b.append(options);
            return *this;
        }
        /** Append a regular expression value
            @param regex the regular expression pattern
            @param regex options such as "i" or "g"
        */
        BSONObjBuilder& appendRegex(string fieldName, string regex, string options = "") {
            return appendRegex(fieldName.c_str(), regex.c_str(), options.c_str());
        }
        BSONObjBuilder& appendCode(const char *fieldName, const char *code) {
            b.append((char) Code);
            b.append(fieldName);
            b.append((int) strlen(code)+1);
            b.append(code);
            return *this;
        }
        /** Append a string element */
        BSONObjBuilder& append(const char *fieldName, const char *str) {
            b.append((char) String);
            b.append(fieldName);
            b.append((int) strlen(str)+1);
            b.append(str);
            return *this;
        }
        /** Append a string element */
        BSONObjBuilder& append(const char *fieldName, string str) {
            return append(fieldName, str.c_str());
        }
        BSONObjBuilder& appendSymbol(const char *fieldName, const char *symbol) {
            b.append((char) Symbol);
            b.append(fieldName);
            b.append((int) strlen(symbol)+1);
            b.append(symbol);
        return *this; }

        /** Append a Null element to the object */
        BSONObjBuilder& appendNull( const char *fieldName ) {
            b.append( (char) jstNULL );
            b.append( fieldName );
        return *this; }

        // Append an element that is less than all other keys.
        BSONObjBuilder& appendMinKey( const char *fieldName ) {
            b.append( (char) MinKey );
            b.append( fieldName );
            return *this; }
        // Append an element that is greater than all other keys.
        BSONObjBuilder& appendMaxKey( const char *fieldName ) {
            b.append( (char) MaxKey );
            b.append( fieldName );
            return *this; }
        
        // Append a Timestamp field -- will be updated to next OpTime on db insert.
        BSONObjBuilder& appendTimestamp( const char *fieldName ) {
            b.append( (char) Timestamp );
            b.append( fieldName );
            b.append( (unsigned long long) 0 );
            return *this; }

        BSONObjBuilder& appendTimestamp( const char *fieldName , unsigned long long val ) {
            b.append( (char) Timestamp );
            b.append( fieldName );
            b.append( val );
            return *this; }

        /**
        Timestamps are a special BSON datatype that is used internally for replication.
        Append a timestamp element to the object being ebuilt.
        @param time - in millis (but stored in seconds)
        */
        BSONObjBuilder& appendTimestamp( const char *fieldName , unsigned long long time , unsigned int inc ){
            OpTime t( (unsigned) (time / 1000) , inc );
            appendTimestamp( fieldName , t.asDate() );
            return *this; 
        }
        
        /*
        Append an element of the deprecated DBRef type.
        @deprecated 
        */
        BSONObjBuilder& appendDBRef( const char *fieldName, const char *ns, const OID &oid ) {
            b.append( (char) DBRef );
            b.append( fieldName );
            b.append( (int) strlen( ns ) + 1 );
            b.append( ns );
            b.append( (void *) &oid, 12 );
            return *this; 
        }

        /** Append a binary data element 
            @param fieldName name of the field
            @param len length of the binary data in bytes
            @param type type information for the data. @see BinDataType.  Use ByteArray if you 
            don't care about the type.
            @param data the byte array
        */
        BSONObjBuilder& appendBinData( const char *fieldName, int len, BinDataType type, const char *data ) {
            b.append( (char) BinData );
            b.append( fieldName );
            b.append( len );
            b.append( (char) type );
            b.append( (void *) data, len );
            return *this; 
        }
        BSONObjBuilder& appendBinData( const char *fieldName, int len, BinDataType type, const unsigned char *data ) {
            return appendBinData(fieldName, len, type, (const char *) data);
        }
        
        /**
        Append a BSON bindata bytearray element.
        @param data a byte array
        @param len the length of data
        */
        BSONObjBuilder& appendBinDataArray( const char * fieldName , const char * data , int len ){
            b.append( (char) BinData );
            b.append( fieldName );
            b.append( len + 4 );
            b.append( (char)0x2 );
            b.append( len );
            b.append( (void *) data, len );            
            return *this; }

        /** Append to the BSON object a field of type CodeWScope.  This is a javascript code 
            fragment accompanied by some scope that goes with it.
        */
        BSONObjBuilder& appendCodeWScope( const char *fieldName, const char *code, const BSONObj &scope ) {
            b.append( (char) CodeWScope );
            b.append( fieldName );
            b.append( ( int )( 4 + 4 + strlen( code ) + 1 + scope.objsize() ) );
            b.append( ( int ) strlen( code ) + 1 );
            b.append( code );
            b.append( ( void * )scope.objdata(), scope.objsize() );
            return *this;
        }

        void appendUndefined( const char *fieldName ) {
            b.append( (char) Undefined );
            b.append( fieldName );
        }
        
        /* helper function -- see Query::where() for primary way to do this. */
        void appendWhere( const char *code, const BSONObj &scope ){
            appendCodeWScope( "$where" , code , scope );
        }
        void appendWhere( const string &code, const BSONObj &scope ){
            appendWhere( code.c_str(), scope );
        }
        
        /**
           these are the min/max when comparing, not strict min/max elements for a given type
        */
        void appendMinForType( const string& field , int type );
        void appendMaxForType( const string& field , int type );

        /** Append an array of values. */
        template < class T >
        BSONObjBuilder& append( const char *fieldName, const vector< T >& vals ) {
            BSONObjBuilder arrBuilder;
            for ( unsigned int i = 0; i < vals.size(); ++i )
                arrBuilder.append( numStr( i ).c_str(), vals[ i ] );
            marshalArray( fieldName, arrBuilder.done() );
            return *this;
        }

        /* Append an array of ints 
           void appendArray( const char *fieldName, const vector< int >& vals ) {
           BSONObjBuilder arrBuilder;
           for ( unsigned i = 0; i < vals.size(); ++i )
           arrBuilder.append( numStr( i ).c_str(), vals[ i ] );
           marshalArray( fieldName, arrBuilder.done() );
           }*/

        /** The returned BSONObj will free the buffer when it is finished. */
        BSONObj obj() {
            massert( 10335 ,  "builder does not own memory", owned() );
            int l;
            return BSONObj(decouple(l), true);
        }

        /** Fetch the object we have built.
			BSONObjBuilder still frees the object when the builder goes out of 
			scope -- very important to keep in mind.  Use obj() if you 
			would like the BSONObj to last longer than the builder.
        */
        BSONObj done() {
            return BSONObj(_done());
        }

        /** Peek at what is in the builder, but leave the builder ready for more appends.
            The returned object is only valid until the next modification or destruction of the builder.
            Intended use case: append a field if not already there.
        */
        BSONObj asTempObj() {
            BSONObj temp(_done());
            b.setlen(b.len()-1); //next append should overwrite the EOO
            return temp;
        }

        /* assume ownership of the buffer - you must then free it (with free()) */
        char* decouple(int& l) {
            char *x = _done();
            assert( x );
            l = b.len();
            b.decouple();
            return x;
        }
        void decouple() {
            b.decouple();    // post done() call version.  be sure jsobj frees...
        }

        void appendKeys( const BSONObj& keyPattern , const BSONObj& values );

    public:
        static string numStr( int i ) {
            if (i>=0 && i<100)
                return numStrs[i];

            stringstream o;
            o << i;
            return o.str();
        }

        /** Stream oriented way to add field names and values. */
        BSONObjBuilderValueStream &operator<<(const char * name ) {
            s_.endField( name );
            return s_;
        }

        /** Stream oriented way to add field names and values. */
        BSONObjBuilder& operator<<( GENOIDLabeler ) { return genOID(); }

        // prevent implicit string conversions which would allow bad things like BSON( BSON( "foo" << 1 ) << 2 )
        struct ForceExplicitString {
            ForceExplicitString( const string &str ) : str_( str ) {}
            string str_;
        };

        /** Stream oriented way to add field names and values. */
        BSONObjBuilderValueStream &operator<<( const ForceExplicitString& name ) {
            return operator<<( name.str_.c_str() );
        }

        Labeler operator<<( const Labeler::Label &l ) {
            massert( 10336 ,  "No subobject started", s_.subobjStarted() );
            return s_ << l;
        }

        bool owned() const {
            return &b == &buf_;
        }

        BSONObjIterator iterator() const ;
        
    private:
        // Append the provided arr object as an array.
        void marshalArray( const char *fieldName, const BSONObj &arr ) {
            b.append( (char) Array );
            b.append( fieldName );
            b.append( (void *) arr.objdata(), arr.objsize() );
        }
        
        char* _done() {
            s_.endField();
            b.append((char) EOO);
            char *data = b.buf() + offset_;
            int size = b.len() - offset_;
            *((int*)data) = size;
            if ( _tracker )
                _tracker->got( size );
            return data;
        }

        BufBuilder &b;
        BufBuilder buf_;
        int offset_;
        BSONObjBuilderValueStream s_;
        BSONSizeTracker * _tracker;

        static const string numStrs[100]; // cache of 0 to 99 inclusive
    };

    class BSONArrayBuilder : boost::noncopyable {
    public:
        BSONArrayBuilder() : _i(0), _b() {}
        BSONArrayBuilder( BufBuilder &b ) : _i(0), _b(b) {}

        template <typename T>
        BSONArrayBuilder& append(const T& x){
            _b.append(num().c_str(), x);
            return *this;
        }

        BSONArrayBuilder& append(const BSONElement& e){
            _b.appendAs(e, num());
            return *this;
        }
        
        template <typename T>
        BSONArrayBuilder& operator<<(const T& x){
            return append(x);
        }
        
        void appendNull() {
            _b.appendNull(num().c_str());
        }

        BSONArray arr(){ return BSONArray(_b.obj()); }
        
        BSONObj done() { return _b.done(); }
        
        template <typename T>
        BSONArrayBuilder& append(const char *name, const T& x){
            fill( name );
            append( x );
            return *this;
        }
        
        BufBuilder &subobjStart( const char *name ) {
            fill( name );
            return _b.subobjStart( num().c_str() );
        }

        BufBuilder &subarrayStart( const char *name ) {
            fill( name );
            return _b.subarrayStart( num().c_str() );
        }
        
        void appendArray( const char *name, BSONObj subObj ) {
            fill( name );
            _b.appendArray( num().c_str(), subObj );
        }
        
        void appendAs( const BSONElement &e, const char *name ) {
            fill( name );
            append( e );
        }
        
    private:
        void fill( const char *name ) {
            char *r;
            int n = strtol( name, &r, 10 );
            uassert( 13048, "can't append to array using string field name", !*r );
            while( _i < n )
                append( nullElt() );
        }
        
        static BSONElement nullElt() {
            static BSONObj n = nullObj();
            return n.firstElement();
        }
        
        static BSONObj nullObj() {
            BSONObjBuilder b;
            b.appendNull( "" );
            return b.obj();
        }
        
        string num(){ return _b.numStr(_i++); }
        int _i;
        BSONObjBuilder _b;
    };

}
