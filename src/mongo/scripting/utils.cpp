// utils.cpp
/*
 *    Copyright (C) 2010 10gen Inc.
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

#include "mongo/scripting/engine.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/version.h"

namespace mongo {

    static BSONObj native_hex_md5( const BSONObj& args, void* data ) {
        uassert( 10261,
                 "hex_md5 takes a single string argument -- hex_md5(string)",
                 args.nFields() == 1 && args.firstElement().type() == String );
        const char * s = args.firstElement().valuestrsafe();

        md5digest d;
        md5_state_t st;
        md5_init(&st);
        md5_append( &st , (const md5_byte_t*)s , strlen( s ) );
        md5_finish(&st, d);

        return BSON( "" << digestToString( d ) );
    }

    static BSONObj native_version( const BSONObj& args, void* data ) {
        return BSON("" << versionString);
    }

    static BSONObj native_sleep( const mongo::BSONObj& args, void* data ) {
        uassert( 16259,
                 "sleep takes a single numeric argument -- sleep(milliseconds)",
                 args.nFields() == 1 && args.firstElement().isNumber() );
        sleepmillis( static_cast<long long>( args.firstElement().number() ) );

        BSONObjBuilder b;
        b.appendUndefined( "" );
        return b.obj();
    }

    // ---------------------------------
    // ---- installer           --------
    // ---------------------------------

    void installGlobalUtils( Scope& scope ) {
        scope.injectNative( "hex_md5" , native_hex_md5 );
        scope.injectNative( "version" , native_version );
        scope.injectNative( "sleep" , native_sleep );
    }

}
