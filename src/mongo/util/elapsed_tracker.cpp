// @file elapsed_tracker.cpp

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

#include "mongo/platform/basic.h"

#include "mongo/util/elapsed_tracker.h"

#include "mongo/util/net/listen.h"

namespace mongo {

    ElapsedTracker::ElapsedTracker( int32_t hitsBetweenMarks, int32_t msBetweenMarks ) :
        _hitsBetweenMarks( hitsBetweenMarks ),
        _msBetweenMarks( msBetweenMarks ),
        _pings( 0 ),
        _last( Listener::getElapsedTimeMillis() ) {
    }

    bool ElapsedTracker::intervalHasElapsed() {
        if ( ( ++_pings % _hitsBetweenMarks ) == 0 ) {
            _last = Listener::getElapsedTimeMillis();
            return true;
        }

        long long now = Listener::getElapsedTimeMillis();
        if ( now - _last > _msBetweenMarks ) {
            _last = now;
            return true;
        }

        return false;
    }

    void ElapsedTracker::resetLastTime() {
        _last = Listener::getElapsedTimeMillis();
    }

} // namespace mongo
