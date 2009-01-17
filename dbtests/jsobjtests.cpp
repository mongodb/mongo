// jsobjtests.cpp - Tests for jsobj.{h,cpp} code
//

/**
 *    Copyright (C) 2008 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "../db/jsobj.h"
#include "../db/json.h"

#include "dbtests.h"

namespace JsobjTests {
    namespace BSONObjTests {
        class Create {
        public:
            void run() {
                BSONObj b;
                ASSERT_EQUALS( 0, b.nFields() );
            }
        };

        class Base {
        protected:
            static BSONObj basic( const char *name, int val ) {
                BSONObjBuilder b;
                b.appendInt( name, val );
                return b.doneAndDecouple();
            }
            static BSONObj basic( const char *name, vector< int > val ) {
                BSONObjBuilder b;
                b.appendIntArray( name, val );
                return b.doneAndDecouple();
            }
            template< class T >
            static BSONObj basic( const char *name, T val ) {
                BSONObjBuilder b;
                b.append( name, val );
                return b.doneAndDecouple();
            }
        };

        class WoCompareBasic : public Base {
        public:
            void run() {
                ASSERT( basic( "a", 1 ).woCompare( basic( "a", 1 ) ) == 0 );
                ASSERT( basic( "a", 2 ).woCompare( basic( "a", 1 ) ) > 0 );
                ASSERT( basic( "a", 1 ).woCompare( basic( "a", 2 ) ) < 0 );
                // field name comparison
                ASSERT( basic( "a", 1 ).woCompare( basic( "b", 1 ) ) < 0 );
            }
        };

        class NumericCompareBasic : public Base {
        public:
            void run() {
                ASSERT( basic( "a", 1 ).woCompare( basic( "a", 1.0 ) ) == 0 );
            }
        };

        class WoCompareEmbeddedObject : public Base {
        public:
            void run() {
                ASSERT( basic( "a", basic( "b", 1 ) ).woCompare
                        ( basic( "a", basic( "b", 1.0 ) ) ) == 0 );
                ASSERT( basic( "a", basic( "b", 1 ) ).woCompare
                        ( basic( "a", basic( "b", 2 ) ) ) < 0 );
            }
        };

        class WoCompareEmbeddedArray : public Base {
        public:
            void run() {
                vector< int > i;
                i.push_back( 1 );
                i.push_back( 2 );
                vector< double > d;
                d.push_back( 1 );
                d.push_back( 2 );
                ASSERT( basic( "a", i ).woCompare( basic( "a", d ) ) == 0 );

                vector< int > j;
                j.push_back( 1 );
                j.push_back( 3 );
                ASSERT( basic( "a", i ).woCompare( basic( "a", j ) ) < 0 );
            }
        };

        class WoCompareOrdered : public Base {
        public:
            void run() {
                ASSERT( basic( "a", 1 ).woCompare( basic( "a", 1 ), basic( "a", 1 ) ) == 0 );
                ASSERT( basic( "a", 2 ).woCompare( basic( "a", 1 ), basic( "a", 1 ) ) > 0 );
                ASSERT( basic( "a", 1 ).woCompare( basic( "a", 2 ), basic( "a", 1 ) ) < 0 );
                ASSERT( basic( "a", 1 ).woCompare( basic( "a", 1 ), basic( "a", -1 ) ) == 0 );
                ASSERT( basic( "a", 2 ).woCompare( basic( "a", 1 ), basic( "a", -1 ) ) < 0 );
                ASSERT( basic( "a", 1 ).woCompare( basic( "a", 2 ), basic( "a", -1 ) ) > 0 );
            }
        };

        namespace Validation {
            
            class Base {
            public:
                void run() {
                    ASSERT( valid().valid() );
                    ASSERT( !invalid().valid() );
                }
            protected:
                virtual BSONObj valid() const = 0;
                virtual BSONObj invalid() const = 0;
                static char get( const BSONObj &o, int i ) {
                    return o.objdata()[ i ];
                }
                static void set( BSONObj &o, int i, char c ) {
                    const_cast< char * >( o.objdata() )[ i ] = c;
                }
            };
            
            class BadType : public Base {
                BSONObj valid() const {
                    return fromjson( "{\"a\":1}" );
                }
                BSONObj invalid() const {
                    BSONObj ret = valid();
                    set( ret, 4, 50 );
                    return ret;
                }
            };

            class EooBeforeEnd : public Base {
                BSONObj valid() const {
                    return fromjson( "{\"a\":1}" );
                }
                BSONObj invalid() const {
                    BSONObj ret = valid();
                    // (first byte of size)++
                    set( ret, 0, get( ret, 0 ) + 1 );
                    // re-read size for BSONObj::details
                    return ret.copy();
                }
            };

            class UndefinedBeforeEnd : public Base {
                BSONObj valid() const {
                    return fromjson( "{\"a\":1}" );
                }
                BSONObj invalid() const {
                    BSONObj ret = valid();
                    set( ret, 4, Undefined );
                    return ret;
                }
            };
            
            class TotalSizeTooSmall : public Base {
                BSONObj valid() const {
                    return fromjson( "{\"a\":1}" );
                }
                BSONObj invalid() const {
                    BSONObj ret = valid();
                    // (first byte of size)--
                    set( ret, 0, get( ret, 0 ) - 1 );
                    // re-read size for BSONObj::details
                    return ret.copy();                    
                }
            };

            class EooMissing : public Base {
                BSONObj valid() const {
                    return fromjson( "{\"a\":1}" );
                }
                BSONObj invalid() const {
                    BSONObj ret = valid();
                    set( ret, ret.objsize() - 1, 0xff );
                    // (first byte of size)--
                    set( ret, 0, get( ret, 0 ) - 1 );
                    // re-read size for BSONObj::details
                    return ret.copy();                    
                }
            };
            
            class WrongStringSize : public Base {
                BSONObj valid() const {
                    return fromjson( "{\"a\":\"b\"}" );
                }
                BSONObj invalid() const {
                    BSONObj ret = valid();
                    set( ret, 0, get( ret, 0 ) + 1 );
                    set( ret, 7, get( ret, 7 ) + 1 );
                    return ret.copy();       
                }
            };

            class WrongSubobjectSize : public Base {
                BSONObj valid() const {
                    return fromjson( "{\"a\":{\"b\":1}}" );
                }
                BSONObj invalid() const {
                    BSONObj ret = valid();
                    set( ret, 0, get( ret, 0 ) + 1 );
                    set( ret, 7, get( ret, 7 ) + 1 );
                    return ret.copy();       
                }
            };            

            class WrongDbrefNsSize : public Base {
                BSONObj valid() const {
                    return fromjson( "{ \"a\": Dbref( \"b\", \"ffffffffffffffffffffffff\" ) }" );
                }
                BSONObj invalid() const {
                    BSONObj ret = valid();
                    set( ret, 0, get( ret, 0 ) + 1 );
                    set( ret, 7, get( ret, 7 ) + 1 );
                    return ret.copy();
                };
            };
            
            class WrongSymbolSize : public Base {
                BSONObj valid() const {
                    return fromjson( "{\"a\":\"b\"}" );
                }
                BSONObj invalid() const {
                    BSONObj ret = valid();
                    set( ret, 4, Symbol );
                    set( ret, 0, get( ret, 0 ) + 1 );
                    set( ret, 7, get( ret, 7 ) + 1 );
                    return ret.copy();       
                }
            };

            class NoFieldNameEnd : public Base {
                BSONObj valid() const {
                    return fromjson( "{\"a\":1}" );
                }
                BSONObj invalid() const {
                    BSONObj ret = valid();
                    memset( const_cast< char * >( ret.objdata() ) + 5, 0xff, ret.objsize() - 5 );
                    return ret;
                }                
            };
            
            class BadRegex : public Base {
                BSONObj valid() const {
                    return fromjson( "{\"a\":/c/i}" );
                }
                BSONObj invalid() const {
                    BSONObj ret = valid();
                    memset( const_cast< char * >( ret.objdata() ) + 7, 0xff, ret.objsize() - 7 );
                    return ret;
                }                
            };
            
            class BadRegexOptions : public Base {
                BSONObj valid() const {
                    return fromjson( "{\"a\":/c/i}" );
                }
                BSONObj invalid() const {
                    BSONObj ret = valid();
                    memset( const_cast< char * >( ret.objdata() ) + 9, 0xff, ret.objsize() - 9 );
                    return ret;
                }                
            };

            class NoSize {
            public:
                NoSize( BSONType type ) : type_( type ) {}
                void run() {
                    const char data[] = { 0x07, 0x00, 0x00, 0x00, char( type_ ), 'a', 0x00 };
                    BSONObj o( data );
                    ASSERT( !o.valid() );
                }
            private:
                BSONType type_;
            };
            
            // Randomized BSON parsing test.  See if we seg fault.
            class Fuzz {
            public:
                Fuzz( double frequency ) : frequency_( frequency ) {}
                void run() {
                    BSONObj o = fromjson( "{\"one\":2, \"two\":5, \"three\": {},"
                                         "\"four\": { \"five\": { \"six\" : 11 } },"
                                         "\"seven\": [ \"a\", \"bb\", \"ccc\", 5 ],"
                                         "\"eight\": Dbref( \"rrr\", \"01234567890123456789aaaa\" ),"
                                         "\"_id\": ObjectId( \"deadbeefdeadbeefdeadbeef\" ),"
                                         "\"nine\": { \"$binary\": \"abc=\", \"$type\": \"02\" },"
                                         "\"ten\": Date( 44 ), \"eleven\": /foooooo/i }" );
                    for( int i = 4; i < o.objsize(); ++i )
                        for( unsigned char j = 1; j; j <<= 1 )
                            if ( rand() < int( RAND_MAX * frequency_ ) ) {
                                char *c = const_cast< char * >( o.objdata() ) + i;
                                if ( *c & j )
                                    *c &= ~j;
                                else
                                    *c |= j;
                            }
                    o.valid();
                }
            private:
                double frequency_;
            };
            
        } // namespace Validation        
        
    } // namespace BSONObjTests
    
    namespace OIDTests {
        class init1 {
        public:
            void run(){
                OID a;
                OID b;
                
                a.init();
                b.init();
                
                ASSERT( a != b );
            }
        };
    }

    class All : public UnitTest::Suite {
    public:
        All() {
            add< BSONObjTests::Create >();
            add< BSONObjTests::WoCompareBasic >();
            add< BSONObjTests::NumericCompareBasic >();
            add< BSONObjTests::WoCompareEmbeddedObject >();
            add< BSONObjTests::WoCompareEmbeddedArray >();
            add< BSONObjTests::WoCompareOrdered >();
            add< BSONObjTests::Validation::BadType >();
            add< BSONObjTests::Validation::EooBeforeEnd >();
            add< BSONObjTests::Validation::UndefinedBeforeEnd >();
            add< BSONObjTests::Validation::TotalSizeTooSmall >();
            add< BSONObjTests::Validation::EooMissing >();
            add< BSONObjTests::Validation::WrongStringSize >();
            add< BSONObjTests::Validation::WrongSubobjectSize >();
            add< BSONObjTests::Validation::WrongDbrefNsSize >();
            add< BSONObjTests::Validation::WrongSymbolSize >();
            add< BSONObjTests::Validation::NoFieldNameEnd >();
            add< BSONObjTests::Validation::BadRegex >();
            add< BSONObjTests::Validation::BadRegexOptions >();
            add< BSONObjTests::Validation::NoSize >( Symbol );
            add< BSONObjTests::Validation::NoSize >( Code );
            add< BSONObjTests::Validation::NoSize >( String );
            add< BSONObjTests::Validation::NoSize >( CodeWScope );
            add< BSONObjTests::Validation::NoSize >( DBRef );
            add< BSONObjTests::Validation::NoSize >( Object );
            add< BSONObjTests::Validation::NoSize >( Array );
            add< BSONObjTests::Validation::NoSize >( BinData );
            add< BSONObjTests::Validation::Fuzz >( .5 );
            add< BSONObjTests::Validation::Fuzz >( .1 );
            add< BSONObjTests::Validation::Fuzz >( .05 );
            add< BSONObjTests::Validation::Fuzz >( .01 );
            add< BSONObjTests::Validation::Fuzz >( .001 );
            add< OIDTests::init1 >();
        }
    };

} // namespace JsobjTests

UnitTest::TestPtr jsobjTests() {
    return UnitTest::createSuite< JsobjTests::All >();
}
