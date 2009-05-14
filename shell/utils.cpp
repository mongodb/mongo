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
        
        BSONObj Quit(const BSONObj& args) {
            // If not arguments are given first element will be EOO, which
            // converts to the integer value 0.
            int exit_code = int( args.firstElement().number() );
            ::exit(exit_code);
            return undefined_;
        }
        
#ifndef _WIN32
        BSONObj AllocatePorts( const BSONObj &args ) {
            uassert( "allocatePorts takes exactly 1 argument", args.nFields() == 1 );
            uassert( "allocatePorts needs to be passed an integer", args.firstElement().isNumber() );
            
            int n = int( args.firstElement().number() );
            
            vector< int > ports;
            for( int i = 0; i < n; ++i ) {
                int s = socket( AF_INET, SOCK_STREAM, 0 );
                assert( s );
                
                sockaddr_in address;
                memset(address.sin_zero, 0, sizeof(address.sin_zero));
                address.sin_family = AF_INET;
                address.sin_port = 0;
                address.sin_addr.s_addr = 0;        
                assert( 0 == ::bind( s, (sockaddr*)&address, sizeof( address ) ) );
                
                sockaddr_in newAddress;
                socklen_t len = sizeof( newAddress );
                assert( 0 == getsockname( s, (sockaddr*)&newAddress, &len ) );
                ports.push_back( ntohs( newAddress.sin_port ) );
                
                assert( 0 == close( s ) );
            }
            
            sort( ports.begin(), ports.end() );
            BSONObjBuilder b;
            b.append( "", ports );
            return b.obj();
        }
#endif
        
            void installShellUtils( Scope& scope ){
            scope.injectNative( "listFiles" , listFiles );
            scope.injectNative( "sleep" , JSSleep );
            scope.injectNative( "quit", Quit );
#if !defined(_WIN32)
            scope.injectNative( "allocatePorts", AllocatePorts );
#endif
        }
        
    }
}
