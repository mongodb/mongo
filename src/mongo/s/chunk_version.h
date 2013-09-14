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
#include "mongo/s/bson_serializable.h"

namespace mongo {

    /**
     * ChunkVersions consist of a major/minor version scoped to a version epoch
     *
     * Version configurations (format: major version, epoch):
     *
     * 1. (0, 0) - collection is dropped.
     * 2. (0, n), n > 0 - applicable only to shardVersion; shard has no chunk.
     * 3. (n, 0), n > 0 - invalid configuration.
     * 4. (n, m), n > 0, m > 0 - normal sharded collection version.
     *
     * TODO: This is a "manual type" but, even so, still needs to comform to what's
     * expected from types.
     */
    struct ChunkVersion : public BSONSerializable {
        static const ChunkVersion DROPPED;

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
            if( ! (*canParse) ) return version;

            *canParse = true;

            if( ! it.more() ) return version;
            BSONElement next = it.next();
            if( next.type() != jstOID ) return version;

            version._epoch = next.OID();

            return version;
        }

        enum VersionChoice {
            VersionChoice_Local,
            VersionChoice_Remote,
            VersionChoice_Unknown
        };

        /**
         * Compares a remotely-loaded version 'remoteVersion' to the latest local version of a
         * collection, 'localVersion', and returns the newest.
         *
         * Because it isn't clear during epoch changes which epoch is newer, the local version
         * before the reload occurred, 'prevLocalVersion', is used to determine whether the remote
         * epoch is definitely newer, or we're not sure.
         */
        static VersionChoice chooseNewestVersion( ChunkVersion prevLocalVersion,
                                                  ChunkVersion localVersion,
                                                  ChunkVersion remoteVersion )
        {
            OID prevEpoch = prevLocalVersion.epoch();
            OID localEpoch = localVersion.epoch();
            OID remoteEpoch = remoteVersion.epoch();

            // Everything changed in-flight, so we need to try again
            if ( prevEpoch != localEpoch && localEpoch != remoteEpoch ) {
                return VersionChoice_Unknown;
            }

            // We're in the same (zero) epoch as the latest metadata, nothing to do
            if ( localEpoch == remoteEpoch && !remoteEpoch.isSet() ) {
                return VersionChoice_Local;
            }

            // We're in the same (non-zero) epoch as the latest metadata, so increment the version
            if ( localEpoch == remoteEpoch && remoteEpoch.isSet() ) {

                // Use the newer version if possible
                if ( localVersion < remoteVersion ) {
                    return VersionChoice_Remote;
                }
                else {
                    return VersionChoice_Local;
                }
            }

            // We're now sure we're installing a new epoch and the epoch didn't change during reload
            dassert( prevEpoch == localEpoch && localEpoch != remoteEpoch );
            return VersionChoice_Remote;
        }

        //
        // Currently our BSON output is to two different fields, to cleanly work with older
        // versions that know nothing about epochs.
        //

        BSONObj toBSONWithPrefix( const string& prefixIn ) const {
            BSONObjBuilder b;

            string prefix = prefixIn;
            if( prefix == "" ) prefix = "version";

            b.appendTimestamp( prefix, _combined );
            b.append( prefix + "Epoch", _epoch );
            return b.obj();
        }

        void addToBSON( BSONObjBuilder& b, const string& prefix="" ) const {
            b.appendElements( toBSONWithPrefix( prefix ) );
        }

        void addEpochToBSON( BSONObjBuilder& b, const string& prefix="" ) const {
            b.append( prefix + "Epoch", _epoch );
        }

        //
        // bson serializable interface implementation
        // (toBSON and toString were implemented above)
        //

        virtual bool isValid(std::string* errMsg) const {
            // TODO is there any check we want to do here?
            return true;
        }

        virtual BSONObj toBSON() const {
            // ChunkVersion wants to be an array.
            BSONArrayBuilder b;
            b.appendTimestamp(_combined);
            b.append(_epoch);
            return b.arr();
        }

        virtual bool parseBSON(const BSONObj& source, std::string* errMsg) {
            // ChunkVersion wants to be an array.
            BSONArray arrSource = static_cast<BSONArray>(source);

            bool canParse;
            ChunkVersion version = fromBSON(arrSource, &canParse);
            if (!canParse) {
                *errMsg = "Could not parse version structure";
                return false;
            }

            _minor = version._minor;
            _major = version._major;
            _epoch = version._epoch;
            return true;
        }

        virtual void clear() {
            _minor = 0;
            _major = 0;
            _epoch = OID();
        }

        void cloneTo(ChunkVersion* other) const {
            other->clear();
            other->_minor = _minor;
            other->_major = _major;
            other->_epoch = _epoch;
        }

    };

    inline ostream& operator<<( ostream &s , const ChunkVersion& v) {
        s << v.toString();
        return s;
    }

} // namespace mongo
