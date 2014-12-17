/* hasher.cpp
 *
 * Defines a simple hash function class
 */


/**
*    Copyright (C) 2012 10gen Inc.
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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/db/hasher.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/startup_test.h"

namespace mongo {

    Hasher::Hasher( HashSeed seed ) : _seed( seed ) {
        md5_init( &_md5State );
        md5_append( &_md5State , reinterpret_cast< const md5_byte_t * >( & _seed ) , sizeof( _seed ) );
    }

    void Hasher::addData( const void * keyData , size_t numBytes ) {
        md5_append( &_md5State , static_cast< const md5_byte_t * >( keyData ), numBytes );
    }

    void Hasher::finish( HashDigest out ) {
        md5_finish( &_md5State , out );
    }

    long long int BSONElementHasher::hash64( const BSONElement& e , HashSeed seed ){
        scoped_ptr<Hasher> h( HasherFactory::createHasher( seed ) );
        recursiveHash( h.get() , e , false );
        HashDigest d;
        h->finish(d);
        //HashDigest is actually 16 bytes, but we just get 8 via truncation
        // NOTE: assumes little-endian
        return *reinterpret_cast< long long int * >( d );
    }

    void BSONElementHasher::recursiveHash( Hasher* h ,
                                           const BSONElement& e ,
                                           bool includeFieldName ) {

        int canonicalType = e.canonicalType();
        h->addData( &canonicalType , sizeof( canonicalType ) );

        if ( includeFieldName ){
            h->addData( e.fieldName() , e.fieldNameSize() );
        }

        if ( !e.mayEncapsulate() ){
            //if there are no embedded objects (subobjects or arrays),
            //compute the hash, squashing numeric types to 64-bit ints
            if ( e.isNumber() ){
                long long int i = e.safeNumberLong(); //well-defined for troublesome doubles
                h->addData( &i , sizeof( i ) );
            }
            else {
                h->addData( e.value() , e.valuesize() );
            }
        }
        else {
            //else identify the subobject.
            //hash any preceding stuff (in the case of codeWscope)
            //then each sub-element
            //then finish with the EOO element.
            BSONObj b;
            if ( e.type() == CodeWScope ) {
                h->addData( e.codeWScopeCode() , e.codeWScopeCodeLen() );
                b = e.codeWScopeObject();
            }
            else {
                b = e.embeddedObject();
            }
            BSONObjIterator i(b);
            while( i.moreWithEOO() ) {
                BSONElement el = i.next();
                recursiveHash( h , el ,  true );
            }
        }
    }

    struct HasherUnitTest : public StartupTest {
        void run() {
            // Hard-coded check to ensure the hash function is consistent across platforms
            BSONObj o = BSON( "check" << 42 );
            verify( BSONElementHasher::hash64( o.firstElement(), 0 ) == -944302157085130861LL );
        }
    } hasherUnitTest;
}
