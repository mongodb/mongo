// utils.cpp

#include <boost/thread/xtime.hpp>

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <assert.h>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#include "../client/dbclient.h"
#include "utils.h"

namespace mongo {

    namespace shellUtils {
        
        // helpers
        
        BSONObj makeUndefined() {
            BSONObjBuilder b;
            b.appendUndefined( "" );
            return b.obj();
        }
        BSONObj undefined_ = makeUndefined();
        
        BSONObj encapsulate( const BSONObj &obj ) {
            return BSON( "" << obj );
        }

        void sleepms( int ms ) {
            boost::xtime xt;
            boost::xtime_get(&xt, boost::TIME_UTC);
            xt.sec += ( ms / 1000 );
            xt.nsec += ( ms % 1000 ) * 1000000;
            if ( xt.nsec >= 1000000000 ) {
                xt.nsec -= 1000000000;
                xt.sec++;
            }
            boost::thread::sleep(xt);    
        }
        
        // real methods


        
        mongo::BSONObj JSSleep(const mongo::BSONObj &args){
            assert( args.nFields() == 1 );
            assert( args.firstElement().isNumber() );
            int ms = int( args.firstElement().number() );
            sleepms( ms );
            return undefined_;
        }

        BSONObj listFiles(const BSONObj& args){
            uassert( "need to specify 1 argument to listFiles" , args.nFields() == 1 );
            
            BSONObjBuilder lst;
            
            string rootname = args.firstElement().valuestrsafe();
            path root( rootname );
            
            directory_iterator end;
            directory_iterator i( root);
            
            int num =0;
            while ( i != end ){
                path p = *i;
                
                BSONObjBuilder b;
                b << "name" << p.string();
                b.appendBool( "isDirectory", is_directory( p ) );
                stringstream ss;
                ss << num;
                string name = ss.str();
                lst.append( name.c_str(), b.done() );
                
                num++;
                i++;
            }
            
            BSONObjBuilder ret;
            ret.appendArray( "", lst.done() );
            return ret.obj();
        }
        
        void installShellUtils( Scope& scope ){
            scope.injectNative( "listFiles" , listFiles );
            scope.injectNative( "sleep" , JSSleep );
        }
        
    }
}
