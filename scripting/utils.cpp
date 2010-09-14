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


#include "pch.h"
#include "engine.h"
#include "../util/md5.hpp"
#include "../util/version.h"

namespace mongo {

    void installBenchmarkSystem( Scope& scope );

    BSONObj jsmd5( const BSONObj &a ){
        uassert( 10261 ,  "js md5 needs a string" , a.firstElement().type() == String );
        const char * s = a.firstElement().valuestrsafe();
        
        md5digest d;
        md5_state_t st;
        md5_init(&st);
        md5_append( &st , (const md5_byte_t*)s , strlen( s ) );
        md5_finish(&st, d);
        
        return BSON( "" << digestToString( d ) );
    }
    
    BSONObj JSVersion( const BSONObj& args ){
        cout << "version: " << versionString << endl;
        if ( strstr( versionString , "+" ) )
            printGitVersion();
        return BSONObj();
    }


    // ---------------------------------
    // ---- installer           --------
    // ---------------------------------

    void installGlobalUtils( Scope& scope ){
        scope.injectNative( "hex_md5" , jsmd5 );
        scope.injectNative( "version" , JSVersion );

        installBenchmarkSystem( scope );
    }

}
        

