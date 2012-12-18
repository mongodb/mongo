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

    //
    // ShardChunkVersions consist of a major/minor version scoped to a version epoch
    //
    struct ShardChunkVersion {
        union {
            struct {
                int _minor;
                int _major;
            };
            unsigned long long _combined;
        };
        OID _epoch;

        ShardChunkVersion() : _minor(0), _major(0), _epoch(OID()) {}

        //
        // Constructors shouldn't have default parameters here, since it's vital we track from
        // here on the epochs of versions, even if not used.
        //

        ShardChunkVersion( int major, int minor, const OID& epoch )
            : _minor(minor),_major(major), _epoch(epoch) {
        }

        ShardChunkVersion( unsigned long long ll, const OID& epoch )
            : _combined( ll ), _epoch(epoch) {
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

        // Incrementing an epoch creates a new, randomly generated identifier
        void incEpoch() {
            _epoch = OID::gen();
            _major = 0;
            _minor = 0;
        }

        // Note: this shouldn't be used as a substitute for version except in specific cases -
        // epochs make versions more complex
        unsigned long long toLong() const {
            return _combined;
        }

        bool isSet() const {
            return _combined > 0;
        }

        bool isEpochSet() const {
            return _epoch.isSet();
        }

        string toString() const {
            stringstream ss;
            // Similar to month/day/year.  For the most part when debugging, we care about major
            // so it's first
            ss << _major << "|" << _minor << "||" << _epoch;
            return ss.str();
        }

        int majorVersion() const { return _major; }
        int minorVersion() const { return _minor; }
        OID epoch() const { return _epoch; }

        //
        // Explicit comparison operators - versions with epochs have non-trivial comparisons.
        // > < operators do not check epoch cases.  Generally if using == we need to handle
        // more complex cases.
        //

        bool operator>( const ShardChunkVersion& otherVersion ) const {
            return this->_combined > otherVersion._combined;
        }

        bool operator>=( const ShardChunkVersion& otherVersion ) const {
            return this->_combined >= otherVersion._combined;
        }

        bool operator<( const ShardChunkVersion& otherVersion ) const {
            return this->_combined < otherVersion._combined;
        }

        bool operator<=( const ShardChunkVersion& otherVersion ) const {
            return this->_combined < otherVersion._combined;
        }

        //
        // Equivalence comparison types.
        //

        // Can we write to this data and not have a problem?
        bool isWriteCompatibleWith( const ShardChunkVersion& otherVersion ) const {
            if( ! hasCompatibleEpoch( otherVersion ) ) return false;
            return otherVersion._major == _major;
        }

        // Is this the same version?
        bool isEquivalentTo( const ShardChunkVersion& otherVersion ) const {
            if( ! hasCompatibleEpoch( otherVersion ) ) return false;
            return otherVersion._combined == _combined;
        }

        // Is this in the same epoch?
        bool hasCompatibleEpoch( const ShardChunkVersion& otherVersion ) const {
            return hasCompatibleEpoch( otherVersion._epoch );
        }

        bool hasCompatibleEpoch( const OID& otherEpoch ) const {
            // TODO : Change logic from eras are not-unequal to eras are equal
            if( otherEpoch.isSet() && _epoch.isSet() && otherEpoch != _epoch ) return false;
            return true;
        }

        //
        // BSON input/output
        //
        // The idea here is to make the BSON input style very flexible right now, so we
        // can then tighten it up in the next version.  We can accept either a BSONObject field
        // with version and epoch, or version and epoch in different fields (either is optional).
        // In this case, epoch always is stored in a field name of the version field name + "Epoch"
        //

        //
        // { version : <TS> } and { version : [<TS>,<OID>] } format
        //

        static bool canParseBSON( const BSONElement& el, const string& prefix="" ){
            bool canParse;
            fromBSON( el, prefix, &canParse );
            return canParse;
        }

        static ShardChunkVersion fromBSON( const BSONElement& el, const string& prefix="" ){
            bool canParse;
            return fromBSON( el, prefix, &canParse );
        }

        static ShardChunkVersion fromBSON( const BSONElement& el,
                                           const string& prefix,
                                           bool* canParse )
        {
            *canParse = true;

            int type = el.type();

            if( type == Array ){
                return fromBSON( BSONArray( el.Obj() ), canParse );
            }

            if( type == jstOID ){
                return ShardChunkVersion( 0, 0, el.OID() );
            }

            if( el.isNumber() ){
                return ShardChunkVersion( static_cast<unsigned long long>( el.numberLong() ), OID() );
            }

            if( type == Timestamp || type == Date ){
                return ShardChunkVersion( el._numberLong(), OID() );
            }

            // Note - we used to throw here, we can't anymore b/c debug builds will be unhappy
            warning() << "can't load version from element type (" << (int)(el.type()) << ") "
                      << el << endl;

            *canParse = false;

            return ShardChunkVersion( 0, OID() );
        }

        //
        // { version : <TS>, versionEpoch : <OID> } object format
        //

        static bool canParseBSON( const BSONObj& obj, const string& prefix="" ){
            bool canParse;
            fromBSON( obj, prefix, &canParse );
            return canParse;
        }

        static ShardChunkVersion fromBSON( const BSONObj& obj, const string& prefix="" ){
            bool canParse;
            return fromBSON( obj, prefix, &canParse );
        }

        static ShardChunkVersion fromBSON( const BSONObj& obj,
                                           const string& prefixIn,
                                           bool* canParse )
        {
            *canParse = true;

            string prefix = prefixIn;
            // "version" doesn't have a "cluster constanst" because that field is never
            // written to the config.
            if( prefixIn == "" && ! obj[ "version" ].eoo() ){
                prefix = (string)"version";
            }
            // TODO: use ChunkType::DEPRECATED_lastmod()
            // NOTE: type_chunk.h includes this file
            else if( prefixIn == "" && ! obj["lastmod"].eoo() ){
                prefix = (string)"lastmod";
            }

            ShardChunkVersion version = fromBSON( obj[ prefix ], prefixIn, canParse );

            if( obj[ prefix + "Epoch" ].type() == jstOID ){
                version._epoch = obj[ prefix + "Epoch" ].OID();
                *canParse = true;
            }

            return version;
        }

        //
        // { version : [<TS>, <OID>] } format
        //

        static bool canParseBSON( const BSONArray& arr ){
            bool canParse;
            fromBSON( arr, &canParse );
            return canParse;
        }

        static ShardChunkVersion fromBSON( const BSONArray& arr ){
            bool canParse;
            return fromBSON( arr, &canParse );
        }

        static ShardChunkVersion fromBSON( const BSONArray& arr,
                                           bool* canParse )
        {
            *canParse = false;

            ShardChunkVersion version;

            BSONObjIterator it( arr );
            if( ! it.more() ) return version;

            version = fromBSON( it.next(), "", canParse );
            if( ! canParse ) return version;

            *canParse = true;

            if( ! it.more() ) return version;
            BSONElement next = it.next();
            if( next.type() != jstOID ) return version;

            version._epoch = next.OID();

            return version;
        }

        //
        // Currently our BSON output is to two different fields, to cleanly work with older
        // versions that know nothing about epochs.
        //

        BSONObj toBSON( const string& prefixIn="" ) const {
            BSONObjBuilder b;

            string prefix = prefixIn;
            if( prefix == "" ) prefix = "version";

            b.appendTimestamp( prefix, _combined );
            b.append( prefix + "Epoch", _epoch );
            return b.obj();
        }

        void addToBSON( BSONObjBuilder& b, const string& prefix="" ) const {
            b.appendElements( toBSON( prefix ) );
        }

        void addEpochToBSON( BSONObjBuilder& b, const string& prefix="" ) const {
            b.append( prefix + "Epoch", _epoch );
        }

    };

    inline ostream& operator<<( ostream &s , const ShardChunkVersion& v) {
        s << v.toString();
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
            : AssertionException( mongoutils::str::stream()
                << raw << " ( ns : " << ( error["ns"].type() == String ? error["ns"].String() : string("<unknown>") )
                << ", received : " << ShardChunkVersion::fromBSON( error, "vReceived" ).toString()
                << ", wanted : " << ShardChunkVersion::fromBSON( error, "vWanted" ).toString()
                << ", " << ( code == SendStaleConfigCode ? "send" : "recv" ) << " )",
                code ),

              _justConnection(justConnection) ,
              // For legacy reasons, we may not always get a namespace here
              _ns( error["ns"].type() == String ? error["ns"].String() : "" ),
              _received( ShardChunkVersion::fromBSON( error, "vReceived" ) ),
              _wanted( ShardChunkVersion::fromBSON( error, "vWanted" ) )
        {}

        // Needs message so when we trace all exceptions on construction we get a useful
        // message
        StaleConfigException() :
            AssertionException( "initializing empty stale config exception object", 0 ) {}

        virtual ~StaleConfigException() throw() {}

        virtual void appendPrefix( stringstream& ss ) const { ss << "stale sharding config exception: "; }

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
