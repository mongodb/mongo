// stop_words.cpp

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

#include <map>
#include <set>
#include <string>

#include "mongo/db/fts/stop_words.h"

#include "mongo/base/init.h"
#include "mongo/platform/unordered_map.h"



namespace mongo {

    namespace fts {

        void loadStopWordMap( std::map< std::string, std::set< std::string > >* m );

        namespace {
            unordered_map<string,StopWords*> STOP_WORDS;
            StopWords* empty = NULL;
        }


        StopWords::StopWords(){
        }

        StopWords::StopWords( const std::set<std::string>& words ) {
            for ( std::set<std::string>::const_iterator i = words.begin(); i != words.end(); ++i )
                _words.insert( *i );
        }

        const StopWords* StopWords::getStopWords( const std::string& langauge ) {
            unordered_map<string,StopWords*>::const_iterator i = STOP_WORDS.find( langauge );
            if ( i == STOP_WORDS.end() )
                return empty;
            return i->second;
        }


        MONGO_INITIALIZER(StopWords)(InitializerContext* context) {
            empty = new StopWords();

            std::map< std::string, std::set< std::string > > raw;
            loadStopWordMap( &raw );
            for ( std::map< std::string, std::set< std::string > >::const_iterator i = raw.begin();
                  i != raw.end();
                  ++i ) {
                STOP_WORDS[i->first] = new StopWords( i->second );
            }
            return Status::OK();
        }

    }

}
