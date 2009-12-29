// database.cpp

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

#include "stdafx.h"
#include "pdfile.h"
#include "database.h"

namespace mongo {

    bool Database::_openAllFiles = false;

    bool Database::setProfilingLevel( int newLevel , string& errmsg ){
        if ( profile == newLevel )
            return true;
        
        if ( newLevel < 0 || newLevel > 2 ){
            errmsg = "profiling level has to be >=0 and <= 2";
            return false;
        }
        
        if ( newLevel == 0 ){
            profile = 0;
            return true;
        }
        
        assert( cc().database() == this );

        if ( ! nsdetails( profileName.c_str() ) ){
            BSONObjBuilder spec;
            spec.appendBool( "capped", true );
            spec.append( "size", 131072.0 );
            if ( ! userCreateNS( profileName.c_str(), spec.done(), errmsg , true ) ){
                return false;
            }
        }
        profile = newLevel;
        return true;
    }

} // namespace mongo
