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

#include <boost/scoped_ptr.hpp>

#include <string>

namespace mongo {

    /** Reports information about a match request. */
    class MatchDetails {
    public:
        MatchDetails();

        void resetOutput();

        // for debugging only
        std::string toString() const;

        // relating to whether or not we had to load the full record

        void setLoadedRecord( bool loadedRecord ) { _loadedRecord = loadedRecord; }

        bool hasLoadedRecord() const { return _loadedRecord; }

        // this name is wrong

        bool needRecord() const { return _elemMatchKeyRequested; }

        // if we need to store the offset into an array where we found the match

        /** Request that an elemMatchKey be recorded. */
        void requestElemMatchKey() { _elemMatchKeyRequested = true; }

        bool hasElemMatchKey() const;
        std::string elemMatchKey() const;

        void setElemMatchKey( const std::string &elemMatchKey );

    private:
        bool _loadedRecord;
        bool _elemMatchKeyRequested;
        boost::scoped_ptr<std::string> _elemMatchKey;
    };
}
