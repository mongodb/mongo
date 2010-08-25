// util.h

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

#pragma once

#include "../pch.h"
#include "../client/dbclient.h"
#include "../db/jsobj.h"

/**
   some generic sharding utils that can be used in mongod or mongos
 */

namespace mongo {

    struct ShardChunkVersion {
        union {
            struct {
                int _minor;
                int _major;
            };
            unsigned long long _combined;
        };
        
        ShardChunkVersion( int major=0, int minor=0 )
            : _minor(minor),_major(major){
        }
        
        ShardChunkVersion( unsigned long long ll )
            : _combined( ll ){
        }
        
        ShardChunkVersion( const BSONElement& e ){
            if ( e.type() == Date || e.type() == Timestamp ){
                _combined = e._numberLong();
            }
            else if ( e.eoo() ){
                _combined = 0;
            }
            else {
                log() << "ShardChunkVersion can't handle type (" << (int)(e.type()) << ") " << e << endl;
                assert(0);
            }
        }

        void inc( bool major ){
            if ( major )
                incMajor();
            else
                incMinor();
        }

        void incMajor() {
            _major++;
            _minor = 0;
        }
        
        void incMinor() {
            _minor++;
        }

        unsigned long long toLong() const {
            return _combined;
        }

        bool isSet() const {
            return _combined > 0;
        }

        string toString() const { 
            stringstream ss; 
            ss << _major << "|" << _minor; 
            return ss.str(); 
        }

        int major() const { return _major; }
        int minor() const { return _minor; }
        
        operator unsigned long long() const { return _combined; }
        
        ShardChunkVersion& operator=( const BSONElement& elem ){
            switch ( elem.type() ){
            case Timestamp:
            case NumberLong:
            case Date:
                _combined = elem._numberLong();
                break;
            case EOO:
                _combined = 0;
                break;
            default:
                assert(0);
            }
            return *this;
        }
    };
    
    inline ostream& operator<<( ostream &s , const ShardChunkVersion& v){
        s << v._major << "|" << v._minor;
        return s;
    }

    /** 
     * your config info for a given shard/chunk is out of date 
     */
    class StaleConfigException : public AssertionException {
    public:
        StaleConfigException( const string& ns , const string& raw , bool justConnection = false )
            : AssertionException( (string)"ns: " + ns + " " + raw , 9996 ) , 
              _justConnection(justConnection) ,
              _ns(ns){
        }
        
        virtual ~StaleConfigException() throw(){}
        
        virtual void appendPrefix( stringstream& ss ) const { ss << "StaleConfigException: "; }
        
        bool justConnection() const { return _justConnection; }
        
        string getns() const { return _ns; }

        static bool parse( const string& big , string& ns , string& raw ){
            string::size_type start = big.find( '[' );
            if ( start == string::npos )
                return false;
            string::size_type end = big.find( ']' ,start );
            if ( end == string::npos )
                return false;
            
            ns = big.substr( start + 1 , ( end - start ) - 1 );
            raw = big.substr( end + 1 );
            return true;
        }
    private:
        bool _justConnection;
        string _ns;
    };

    bool checkShardVersion( DBClientBase & conn , const string& ns , bool authoritative = false , int tryNumber = 1 );
    void resetShardVersion( DBClientBase * conn );
}
