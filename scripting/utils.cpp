// utils.cpp

#include "../stdafx.h"
#include "engine.h"
#include "../util/md5.hpp"

namespace mongo {

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

    void installGlobalUtils( Scope& scope ){
        scope.injectNative( "hex_md5" , jsmd5 );
        scope.injectNative( "version" , JSVersion );
    }

}
        

