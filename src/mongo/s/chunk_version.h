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
*/

#pragma once

#include "mongo/db/jsobj.h"

namespace mongo {

    //
    // ChunkVersions consist of a major/minor version scoped to a version epoch
    //
    struct ChunkVersion {
        union {
            struct {
                int _minor;
                int _major;
            };
            unsigned long long _combined;
        };
        OID _epoch;

        ChunkVersion() : _minor(0), _major(0), _epoch(OID()) {}

        //
        // Constructors shouldn't have default parameters here, since it's vital we track from
        // here on the epochs of versions, even if not used.
        //

        ChunkVersion( int major, int minor, const OID& epoch )
            : _minor(minor),_major(major), _epoch(epoch) {
        }

        ChunkVersion( unsigned long long ll, const OID& epoch )
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

        bool operator>( const ChunkVersion& otherVersion ) const {
            return this->_combined > otherVersion._combined;
        }

        bool operator>=( const ChunkVersion& otherVersion ) const {
            return this->_combined >= otherVersion._combined;
        }

        bool operator<( const ChunkVersion& otherVersion ) const {
            return this->_combined < otherVersion._combined;
        }

        bool operator<=( const ChunkVersion& otherVersion ) const {
            return this->_combined <= otherVersion._combined;
        }

        //
        // Equivalence comparison types.
        //

        // Can we write to this data and not have a problem?
        bool isWriteCompatibleWith( const ChunkVersion& otherVersion ) const {
            if( ! hasCompatibleEpoch( otherVersion ) ) return false;
            return otherVersion._major == _major;
        }

        // Is this the same version?
        bool isEquivalentTo( const ChunkVersion& otherVersion ) const {
            if( ! hasCompatibleEpoch( otherVersion ) ) return false;
            return otherVersion._combined == _combined;
        }

        // Is this in the same epoch?
        bool hasCompatibleEpoch( const ChunkVersion& otherVersion ) const {
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

        static ChunkVersion fromBSON( const BSONElement& el, const string& prefix="" ){
            bool canParse;
            return fromBSON( el, prefix, &canParse );
        }

        static ChunkVersion fromBSON( const BSONElement& el,
                                      const string& prefix,
                                      bool* canParse )
        {
            *canParse = true;

            int type = el.type();

            if( type == Array ){
                return fromBSON( BSONArray( el.Obj() ), canParse );
            }

            if( type == jstOID ){
                return ChunkVersion( 0, 0, el.OID() );
            }

            if( el.isNumber() ){
                return ChunkVersion( static_cast<unsigned long long>(el.numberLong()), OID() );
            }

            if( type == Timestamp || type == Date ){
                return ChunkVersion( el._numberLong(), OID() );
            }

            *canParse = false;

            return ChunkVersion( 0, OID() );
        }

        //
        // { version : <TS>, versionEpoch : <OID> } object format
        //

        static bool canParseBSON( const BSONObj& obj, const string& prefix="" ){
            bool canParse;
            fromBSON( obj, prefix, &canParse );
            return canParse;
        }

        static ChunkVersion fromBSON( const BSONObj& obj, const string& prefix="" ){
            bool canParse;
            return fromBSON( obj, prefix, &canParse );
        }

        static ChunkVersion fromBSON( const BSONObj& obj,
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

            ChunkVersion version = fromBSON( obj[ prefix ], prefixIn, canParse );

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

        static ChunkVersion fromBSON( const BSONArray& arr ){
            bool canParse;
            return fromBSON( arr, &canParse );
        }

        static ChunkVersion fromBSON( const BSONArray& arr,
                                      bool* canParse )
        {
            *canParse = false;

            ChunkVersion version;

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

    inline ostream& operator<<( ostream &s , const ChunkVersion& v) {
        s << v.toString();
        return s;
    }

} // namespace mongo
