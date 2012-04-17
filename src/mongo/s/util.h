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

#include "mongo/pch.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/mongoutils/str.h"
/**
   some generic sharding utils that can be used in mongod or mongos
 */

namespace mongo {

    struct ShardChunkVersion {
        union {
            struct {
#ifdef BOOST_LITTLE_ENDIAN
                int _minor;
                int _major;
#else
                int _major;
                int _minor;
#endif
            };
            unsigned long long _combined;
        };

        ShardChunkVersion( int major=0, int minor=0 ) {
            _minor = minor;
            _major = major;
        }

        ShardChunkVersion( unsigned long long ll )
            : _combined( ll ) {
        }

        ShardChunkVersion( const BSONElement& e ) {
            if ( e.type() == Date || e.type() == Timestamp ) {
                _combined = e._numberLong();
            }
            else if ( e.eoo() ) {
                _combined = 0;
            }
            else {
                _combined = 0;
                log() << "ShardChunkVersion can't handle type (" << (int)(e.type()) << ") " << e << endl;
                verify(0);
            }
        }

        void inc( bool major ) {
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

        int majorVersion() const { return _major; }
        int minorVersion() const { return _minor; }

        operator unsigned long long() const { return _combined; }

        ShardChunkVersion& operator=( const BSONElement& elem ) {
            switch ( elem.type() ) {
            case Timestamp:
            case NumberLong:
            case Date:
                _combined = elem._numberLong();
                break;
            case EOO:
                _combined = 0;
                break;
            default:
                massert( 13657 , mongoutils::str::stream() << "unknown type for ShardChunkVersion: " << elem , 0 );
            }
            return *this;
        }
    };

    inline ostream& operator<<( ostream &s , const ShardChunkVersion& v) {
        s << v._major << "|" << v._minor;
        return s;
    }

    /**
     * your config info for a given shard/chunk is out of date
     */
    class StaleConfigException : public AssertionException {
    public:
        StaleConfigException( const string& ns , const string& raw , int code, ShardChunkVersion received, ShardChunkVersion wanted, bool justConnection = false )
            : AssertionException(
                    mongoutils::str::stream() << raw << " ( ns : " << ns <<
                                             ", received : " << received.toString() <<
                                             ", wanted : " << wanted.toString() <<
                                             ", " << ( code == SendStaleConfigCode ? "send" : "recv" ) << " )",
                    code ),
              _justConnection(justConnection) ,
              _ns(ns),
              _received( received ),
              _wanted( wanted )
        {}

        // Preferred if we're rebuilding this from a thrown exception
        StaleConfigException( const string& raw , int code, const BSONObj& error, bool justConnection = false )
            : AssertionException(
                    mongoutils::str::stream() << raw << " ( ns : " << error["ns"].String() << // Note, this will fail if we don't have a ns
                                             ", received : " << ShardChunkVersion( error["vReceived"] ).toString() <<
                                             ", wanted : " << ShardChunkVersion( error["vWanted"] ).toString() <<
                                             ", " << ( code == SendStaleConfigCode ? "send" : "recv" ) << " )",
                    code ),
              _justConnection(justConnection) ,
              _ns( error["ns"].String() ),
              _received( ShardChunkVersion( error["vReceived"] ) ),
              _wanted( ShardChunkVersion( error["vWanted"] ) )
        {}

        StaleConfigException() : AssertionException( "", 0 ) {}

        virtual ~StaleConfigException() throw() {}

        virtual void appendPrefix( stringstream& ss ) const { ss << "stale sharding config exception: "; }

        bool justConnection() const { return _justConnection; }

        string getns() const { return _ns; }

        static bool parse( const string& big , string& ns , string& raw ) {
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

        ShardChunkVersion getVersionReceived() const { return _received; }
        ShardChunkVersion getVersionWanted() const { return _wanted; }

        StaleConfigException& operator=( const StaleConfigException& elem ) {

            this->_ei.msg = elem._ei.msg;
            this->_ei.code = elem._ei.code;
            this->_justConnection = elem._justConnection;
            this->_ns = elem._ns;
            this->_received = elem._received;
            this->_wanted = elem._wanted;

            return *this;
        }

    private:
        bool _justConnection;
        string _ns;
        ShardChunkVersion _received;
        ShardChunkVersion _wanted;
    };

    class SendStaleConfigException : public StaleConfigException {
    public:
        SendStaleConfigException( const string& ns , const string& raw , ShardChunkVersion received, ShardChunkVersion wanted, bool justConnection = false )
            : StaleConfigException( ns, raw, SendStaleConfigCode, received, wanted, justConnection ) {}
        SendStaleConfigException( const string& raw , const BSONObj& error, bool justConnection = false )
            : StaleConfigException( raw, SendStaleConfigCode, error, justConnection ) {}
    };

    class RecvStaleConfigException : public StaleConfigException {
    public:
        RecvStaleConfigException( const string& ns , const string& raw , ShardChunkVersion received, ShardChunkVersion wanted, bool justConnection = false )
            : StaleConfigException( ns, raw, RecvStaleConfigCode, received, wanted, justConnection ) {}
        RecvStaleConfigException( const string& raw , const BSONObj& error, bool justConnection = false )
            : StaleConfigException( raw, RecvStaleConfigCode, error, justConnection ) {}
    };

    class ShardConnection;
    class DBClientBase;
    class VersionManager {
    public:
        VersionManager(){};

        bool isVersionableCB( DBClientBase* );
        bool initShardVersionCB( DBClientBase*, BSONObj& );
        bool forceRemoteCheckShardVersionCB( const string& );
        bool checkShardVersionCB( DBClientBase*, const string&, bool, int );
        bool checkShardVersionCB( ShardConnection*, bool, int );
        void resetShardVersionCB( DBClientBase* );

    };

    extern VersionManager versionManager;

}
