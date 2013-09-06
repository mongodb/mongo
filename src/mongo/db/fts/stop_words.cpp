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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
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
