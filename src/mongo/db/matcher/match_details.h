// match_details.h

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

#pragma once

#include <string>

namespace mongo {

    /** Reports information about a match request. */
    class MatchDetails {
    public:
        MatchDetails();
        void resetOutput();
        std::string toString() const;

        /** Request that an elemMatchKey be recorded. */
        void requestElemMatchKey() { _elemMatchKeyRequested = true; }

        bool needRecord() const { return _elemMatchKeyRequested; }

        bool hasLoadedRecord() const { return _loadedRecord; }
        bool hasElemMatchKey() const { return _elemMatchKeyFound; }
        std::string elemMatchKey() const {
            verify( hasElemMatchKey() );
            return _elemMatchKey;
        }

        void setLoadedRecord( bool loadedRecord ) { _loadedRecord = loadedRecord; }
        void setElemMatchKey( const std::string &elemMatchKey ) {
            if ( _elemMatchKeyRequested ) {
                _elemMatchKeyFound = true;
                _elemMatchKey = elemMatchKey;
            }
        }

    private:
        bool _loadedRecord;
        bool _elemMatchKeyRequested;
        bool _elemMatchKeyFound;
        std::string _elemMatchKey;
    };
}
