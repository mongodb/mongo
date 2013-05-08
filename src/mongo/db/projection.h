// projection.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include "mongo/pch.h"
#include "mongo/util/string_map.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher.h"

namespace mongo {

    /**
     * given a document and a projection specification
     * can transform the document
     * currently supports specifying which fields and $slice
     */
    class Projection {
    public:

        class KeyOnly {
        public:

            KeyOnly() : _stringSize(0) {}

            BSONObj hydrate( const BSONObj& key ) const;

            void addNo() { _add( false , "" ); }
            void addYes( const string& name ) { _add( true , name ); }

        private:

            void _add( bool b , const string& name ) {
                _include.push_back( b );
                _names.push_back( name );
                _stringSize += name.size();
            }

            vector<bool> _include; // one entry per field in key.  true iff should be in output
            vector<string> _names; // name of field since key doesn't have names

            int _stringSize;
        };

        enum ArrayOpType {
            ARRAY_OP_NORMAL = 0,
            ARRAY_OP_ELEM_MATCH,
            ARRAY_OP_POSITIONAL
        };

        Projection() :
            _include(true) ,
            _special(false) ,
            _includeID(true) ,
            _skip(0) ,
            _limit(-1) ,
            _arrayOpType(ARRAY_OP_NORMAL),
            _hasNonSimple(false) {
        }

        /**
         * called once per lifetime
         * e.g. { "x" : 1 , "a.y" : 1 }
         */
        void init( const BSONObj& spec );

        /**
         * @return the spec init was called with
         */
        BSONObj getSpec() const { return _source; }

        /**
         * transforms in according to spec
         */
        BSONObj transform( const BSONObj& in, const MatchDetails* details = NULL ) const;


        /**
         * transforms in according to spec
         */
        void transform( const BSONObj& in , BSONObjBuilder& b, const MatchDetails* details = NULL ) const;


        /**
         * @return if the keyPattern has all the information needed to return then
         *         return a new KeyOnly otherwise null
         *         NOTE: a key may have modified the actual data
         *               which has to be handled above this (arrays, geo)
         */
        KeyOnly* checkKey( const BSONObj& keyPattern ) const;

        bool includeID() const { return _includeID; }

        /**
         *  get the type of array operator for the projection
         *  @return     ARRAY_OP_NORMAL if no array projection modifier,
         *              ARRAY_OP_ELEM_MATCH if one or more $elemMatch specifier,
         *              ARRAY_OP_POSITIONAL if one '.$' projection specified
         */
        ArrayOpType getArrayOpType() const;

        /**
         * Validate the given query satisfies this projection's positional operator.
         * NOTE: this function is only used to validate projections with a positional operator.
         * @param   query       User-supplied query specifier
         * @return  Field name if found, empty string otherwise.
         */
        void validateQuery( const BSONObj query ) const;

    private:

        /**
         * appends e to b if user wants it
         * will descend into e if needed
         */
        void append( BSONObjBuilder& b , const BSONElement& e, const MatchDetails* details = NULL,
                     const ArrayOpType arrayOpType = ARRAY_OP_NORMAL ) const;

        void add( const string& field, bool include );
        void add( const string& field, int skip, int limit );
        void appendArray( BSONObjBuilder& b , const BSONObj& a , bool nested=false) const;

        bool _include; // true if default at this level is to include
        bool _special; // true if this level can't be skipped or included without recursing

        //TODO: benchmark vector<pair> vs map
        typedef StringMap<boost::shared_ptr<Projection> > FieldMap;
        FieldMap _fields;
        BSONObj _source;
        bool _includeID;

        // used for $slice operator
        int _skip;
        int _limit;

        // used for $elemMatch and positional operator ($)
        typedef StringMap<boost::shared_ptr<Matcher> > Matchers;
        Matchers _matchers;
        ArrayOpType _arrayOpType;

        bool _hasNonSimple;
    };


}
