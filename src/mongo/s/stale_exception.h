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
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#pragma once

#include "mongo/db/jsobj.h"
#include "mongo/s/chunk_version.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    /**
     * Thrown whenever your config info for a given shard/chunk is out of date.
     */
    class StaleConfigException : public AssertionException {
    public:
        StaleConfigException( const string& ns,
                              const string& raw,
                              int code,
                              ChunkVersion received,
                              ChunkVersion wanted,
                              bool justConnection = false )
            : AssertionException(stream() << raw << " ( ns : " << ns
                                          << ", received : " << received.toString()
                                          << ", wanted : " << wanted.toString()
                                          << ", " << ( code == SendStaleConfigCode ?
                                                       "send" : "recv" ) << " )",
                                 code ),
              _justConnection(justConnection),
              _ns(ns),
              _received( received ),
              _wanted( wanted ) {
        }

        /** Preferred if we're rebuilding this from a thrown exception */
        StaleConfigException( const string& raw,
                              int code,
                              const BSONObj& error,
                              bool justConnection = false )
            : AssertionException( stream() << raw << " ( ns : "
                                           << ( error["ns"].type() == String ?
                                                error["ns"].String() : string("<unknown>") )
                                           << ", received : "
                                           << ChunkVersion::fromBSON( error, "vReceived" ).toString()
                                           << ", wanted : "
                                           << ChunkVersion::fromBSON( error, "vWanted" ).toString()
                                           << ", "
                                           << ( code == SendStaleConfigCode ?
                                                "send" : "recv" ) << " )",
                                  code ),
              _justConnection(justConnection) ,
              // For legacy reasons, we may not always get a namespace here
              _ns( error["ns"].type() == String ? error["ns"].String() : "" ),
              _received( ChunkVersion::fromBSON( error, "vReceived" ) ),
              _wanted( ChunkVersion::fromBSON( error, "vWanted" ) ) {
        }

        /**
         * Needs message so when we trace all exceptions on construction we get a useful
         * message
         */
        StaleConfigException() :
            AssertionException( "initializing empty stale config exception object", 0 ) {
        }

        virtual ~StaleConfigException() throw() {}

        virtual void appendPrefix( stringstream& ss ) const {
            ss << "stale sharding config exception: ";
        }

        bool justConnection() const { return _justConnection; }

        string getns() const { return _ns; }

        /**
         * true if this exception would require a full reload of config data to resolve
         */
        bool requiresFullReload() const {
            return ! _received.hasCompatibleEpoch( _wanted ) ||
                     _received.isSet() != _wanted.isSet();
        }

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

        ChunkVersion getVersionReceived() const {
            return _received;
        }

        ChunkVersion getVersionWanted() const {
            return _wanted;
        }

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
        ChunkVersion _received;
        ChunkVersion _wanted;
    };

    class SendStaleConfigException : public StaleConfigException {
    public:
        SendStaleConfigException( const string& ns,
                                  const string& raw,
                                  ChunkVersion received,
                                  ChunkVersion wanted,
                                  bool justConnection = false )
            : StaleConfigException( ns, raw, SendStaleConfigCode, received, wanted, justConnection ){
        }

        SendStaleConfigException( const string& raw,
                                  const BSONObj& error,
                                  bool justConnection = false )
            : StaleConfigException( raw, SendStaleConfigCode, error, justConnection ) {
        }
    };

    class RecvStaleConfigException : public StaleConfigException {
    public:
        RecvStaleConfigException( const string& ns,
                                  const string& raw,
                                  ChunkVersion received,
                                  ChunkVersion wanted,
                                  bool justConnection = false )
            : StaleConfigException( ns, raw, RecvStaleConfigCode, received, wanted, justConnection ){
        }

        RecvStaleConfigException( const string& raw,
                                  const BSONObj& error,
                                  bool justConnection = false )
            : StaleConfigException( raw, RecvStaleConfigCode, error, justConnection ) {
        }
    };

} // namespace mongo
