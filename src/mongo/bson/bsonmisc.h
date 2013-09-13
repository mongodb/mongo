// @file bsonmisc.h

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

#include <memory>

#include "mongo/bson/bsonelement.h"

namespace mongo {

    int getGtLtOp(const BSONElement& e);

    struct BSONElementCmpWithoutField {
        bool operator()( const BSONElement &l, const BSONElement &r ) const {
            return l.woCompare( r, false ) < 0;
        }
    };

    class BSONObjCmp {
    public:
        BSONObjCmp( const BSONObj &order = BSONObj() ) : _order( order ) {}
        bool operator()( const BSONObj &l, const BSONObj &r ) const {
            return l.woCompare( r, _order ) < 0;
        }
        BSONObj order() const { return _order; }
    private:
        BSONObj _order;
    };

    typedef std::set<BSONObj,BSONObjCmp> BSONObjSet;

    enum FieldCompareResult {
        LEFT_SUBFIELD = -2,
        LEFT_BEFORE = -1,
        SAME = 0,
        RIGHT_BEFORE = 1 ,
        RIGHT_SUBFIELD = 2
    };

    class LexNumCmp;
    FieldCompareResult compareDottedFieldNames( const std::string& l , const std::string& r ,
                                               const LexNumCmp& cmp );

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
#define BSON(x) (( ::mongo::BSONObjBuilder(64) << x ).obj())

    /** Use BSON_ARRAY macro like BSON macro, but without keys

        BSONArray arr = BSON_ARRAY( "hello" << 1 << BSON( "foo" << BSON_ARRAY( "bar" << "baz" << "qux" ) ) );

     */
#define BSON_ARRAY(x) (( ::mongo::BSONArrayBuilder() << x ).arr())

    /* Utility class to auto assign object IDs.
       Example:
         std::cout << BSON( GENOID << "z" << 3 ); // { _id : ..., z : 3 }
    */
    extern struct GENOIDLabeler { } GENOID;

    /* Utility class to add a Date element with the current time
       Example:
         std::cout << BSON( "created" << DATENOW ); // { created : "2009-10-09 11:41:42" }
    */
    extern struct DateNowLabeler { } DATENOW;

    /* Utility class to assign a NULL value to a given attribute
       Example:
         std::cout << BSON( "a" << BSONNULL ); // { a : null }
    */
    extern struct NullLabeler { } BSONNULL;

    /* Utility class to assign an Undefined value to a given attribute
       Example:
         std::cout << BSON( "a" << BSONUndefined ); // { a : undefined }
    */
    extern struct UndefinedLabeler { } BSONUndefined;

    /* Utility class to add the minKey (minus infinity) to a given attribute
       Example:
         std::cout << BSON( "a" << MINKEY ); // { "a" : { "$minKey" : 1 } }
    */
    extern struct MinKeyLabeler { } MINKEY;
    extern struct MaxKeyLabeler { } MAXKEY;

    // Utility class to implement GT, GTE, etc as described above.
    class Labeler {
    public:
        struct Label {
            explicit Label( const char *l ) : l_( l ) {}
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

    // Utility class to allow adding a string to BSON as a Symbol
    struct BSONSymbol {
        explicit BSONSymbol(const StringData& sym) :symbol(sym) {}
        StringData symbol;
    };

    // Utility class to allow adding a string to BSON as Code
    struct BSONCode {
        explicit BSONCode(const StringData& str) :code(str) {}
        StringData code;
    };

    // Utility class to allow adding CodeWScope to BSON
    struct BSONCodeWScope {
        explicit BSONCodeWScope(const StringData& str, const BSONObj& obj) :code(str), scope(obj) {}
        StringData code;
        BSONObj scope;
    };

    // Utility class to allow adding a RegEx to BSON
    struct BSONRegEx {
        explicit BSONRegEx(const StringData& pat, const StringData& f="") :pattern(pat), flags(f) {}
        StringData pattern;
        StringData flags;
    };

    // Utility class to allow adding binary data to BSON
    struct BSONBinData {
        BSONBinData(const void* d, int l, BinDataType t) :data(d), length(l), type(t) {}
        const void* data;
        int length;
        BinDataType type;
    };

    // Utility class to allow adding deprecated DBRef type to BSON
    struct BSONDBRef {
        BSONDBRef(const StringData& nameSpace, const OID& o) :ns(nameSpace), oid(o) {}
        StringData ns;
        OID oid;
    };

    extern Labeler::Label GT;
    extern Labeler::Label GTE;
    extern Labeler::Label LT;
    extern Labeler::Label LTE;
    extern Labeler::Label NE;
    extern Labeler::Label NIN;
    extern Labeler::Label BSIZE;


    // $or helper: OR(BSON("x" << GT << 7), BSON("y" << LT << 6));
    // becomes   : {$or: [{x: {$gt: 7}}, {y: {$lt: 6}}]}
    inline BSONObj OR(const BSONObj& a, const BSONObj& b);
    inline BSONObj OR(const BSONObj& a, const BSONObj& b, const BSONObj& c);
    inline BSONObj OR(const BSONObj& a, const BSONObj& b, const BSONObj& c, const BSONObj& d);
    inline BSONObj OR(const BSONObj& a, const BSONObj& b, const BSONObj& c, const BSONObj& d, const BSONObj& e);
    inline BSONObj OR(const BSONObj& a, const BSONObj& b, const BSONObj& c, const BSONObj& d, const BSONObj& e, const BSONObj& f);
    // definitions in bsonobjbuilder.h b/c of incomplete types

    // Utility class to implement BSON( key << val ) as described above.
    class BSONObjBuilderValueStream : public boost::noncopyable {
    public:
        friend class Labeler;
        BSONObjBuilderValueStream( BSONObjBuilder * builder );

        BSONObjBuilder& operator<<( const BSONElement& e );

        template<class T>
        BSONObjBuilder& operator<<( T value );

        BSONObjBuilder& operator<<(const DateNowLabeler& id);

        BSONObjBuilder& operator<<(const NullLabeler& id);
        BSONObjBuilder& operator<<(const UndefinedLabeler& id);

        BSONObjBuilder& operator<<(const MinKeyLabeler& id);
        BSONObjBuilder& operator<<(const MaxKeyLabeler& id);

        Labeler operator<<( const Labeler::Label &l );

        void endField( const StringData& nextFieldName = StringData() );
        bool subobjStarted() const { return _fieldName != 0; }

        // The following methods provide API compatibility with BSONArrayBuilder
        BufBuilder& subobjStart();
        BufBuilder& subarrayStart();

        // This method should only be called from inside of implementations of
        // BSONObjBuilder& operator<<(BSONObjBuilderValueStream&, SOME_TYPE)
        // to provide the return value.
        BSONObjBuilder& builder() { return *_builder; }
    private:
        StringData _fieldName;
        BSONObjBuilder * _builder;

        bool haveSubobj() const { return _subobj.get() != 0; }
        BSONObjBuilder *subobj();
        std::auto_ptr< BSONObjBuilder > _subobj;
    };

    /**
       used in conjuction with BSONObjBuilder, allows for proper buffer size to prevent crazy memory usage
     */
    class BSONSizeTracker {
    public:
        BSONSizeTracker() {
            _pos = 0;
            for ( int i=0; i<SIZE; i++ )
                _sizes[i] = 512; // this is the default, so just be consistent
        }

        ~BSONSizeTracker() {
        }

        void got( int size ) {
            _sizes[_pos] = size;
            _pos = (_pos + 1) % SIZE; // thread safe at least on certain compilers
        }

        /**
         * right now choosing largest size
         */
        int getSize() const {
            int x = 16; // sane min
            for ( int i=0; i<SIZE; i++ ) {
                if ( _sizes[i] > x )
                    x = _sizes[i];
            }
            return x;
        }

    private:
        enum { SIZE = 10 };
        int _pos;
        int _sizes[SIZE];
    };

    // considers order
    bool fieldsMatch(const BSONObj& lhs, const BSONObj& rhs);
}
