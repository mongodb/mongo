// match_details.cpp

/**
*    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/matcher/match_details.h"

#include <sstream>

#include "mongo/util/assert_util.h"

namespace mongo {

    MatchDetails::MatchDetails() :
        _elemMatchKeyRequested() {
        resetOutput();
    }

    void MatchDetails::resetOutput() {
        _loadedRecord = false;
        _elemMatchKey.reset();
    }

    bool MatchDetails::hasElemMatchKey() const {
        return _elemMatchKey.get();
    }

    std::string MatchDetails::elemMatchKey() const {
        verify( hasElemMatchKey() );
        return *(_elemMatchKey.get());
    }

    void MatchDetails::setElemMatchKey( const std::string &elemMatchKey ) {
        if ( _elemMatchKeyRequested ) {
            _elemMatchKey.reset( new std::string( elemMatchKey ) );
        }
    }

    string MatchDetails::toString() const {
        std::stringstream ss;
        ss << "loadedRecord: " << _loadedRecord << " ";
        ss << "elemMatchKeyRequested: " << _elemMatchKeyRequested << " ";
        ss << "elemMatchKey: " << ( _elemMatchKey ? _elemMatchKey->c_str() : "NONE" ) << " ";
        return ss.str();
    }

}
