// embedded_builder.h

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

namespace mongo {

    // utility class for assembling hierarchical objects
    class EmbeddedBuilder {
    public:
        EmbeddedBuilder( BSONObjBuilder *b ) {
            _builders.push_back( make_pair( "", b ) );
        }
        // It is assumed that the calls to prepareContext will be made with the 'name'
        // parameter in lex ascending order.
        void prepareContext( string &name ) {
            int i = 1, n = _builders.size();
            while( i < n &&
                    name.substr( 0, _builders[ i ].first.length() ) == _builders[ i ].first &&
                    ( name[ _builders[i].first.length() ] == '.' || name[ _builders[i].first.length() ] == 0 )
                 ) {
                name = name.substr( _builders[ i ].first.length() + 1 );
                ++i;
            }
            for( int j = n - 1; j >= i; --j ) {
                popBuilder();
            }
            for( string next = splitDot( name ); !next.empty(); next = splitDot( name ) ) {
                addBuilder( next );
            }
        }
        void appendAs( const BSONElement &e, string name ) {
            if ( e.type() == Object && e.valuesize() == 5 ) { // empty object -- this way we can add to it later
                string dummyName = name + ".foo";
                prepareContext( dummyName );
                return;
            }
            prepareContext( name );
            back()->appendAs( e, name );
        }
        BufBuilder &subarrayStartAs( string name ) {
            prepareContext( name );
            return back()->subarrayStart( name );
        }
        void done() {
            while( ! _builderStorage.empty() )
                popBuilder();
        }

        static string splitDot( string & str ) {
            size_t pos = str.find( '.' );
            if ( pos == string::npos )
                return "";
            string ret = str.substr( 0, pos );
            str = str.substr( pos + 1 );
            return ret;
        }

    private:
        void addBuilder( const string &name ) {
            shared_ptr< BSONObjBuilder > newBuilder( new BSONObjBuilder( back()->subobjStart( name ) ) );
            _builders.push_back( make_pair( name, newBuilder.get() ) );
            _builderStorage.push_back( newBuilder );
        }
        void popBuilder() {
            back()->done();
            _builders.pop_back();
            _builderStorage.pop_back();
        }

        BSONObjBuilder *back() { return _builders.back().second; }

        vector< pair< string, BSONObjBuilder * > > _builders;
        vector< shared_ptr< BSONObjBuilder > > _builderStorage;

    };

} //namespace mongo
