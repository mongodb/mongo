/**
*    Copyright (C) 2009 10gen Inc.
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

#include "pch.h"
#include "curop.h"

namespace mongo {

    // todo : move more here

    CurOp::CurOp( Client * client , CurOp * wrapped ) {
        _client = client;
        _wrapped = wrapped;
        if ( _wrapped )
            _client->_curOp = this;
        _start = _checkpoint = 0;
        _active = false;
        _reset();
        _op = 0;
        // These addresses should never be written to again.  The zeroes are
        // placed here as a precaution because currentOp may be accessed
        // without the db mutex.
        memset(_ns, 0, sizeof(_ns));
    }

}
