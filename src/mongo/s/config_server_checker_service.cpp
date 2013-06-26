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

#include <boost/scoped_ptr.hpp>
#include <boost/thread/thread.hpp>

#include "mongo/s/config.h"
#include "mongo/s/config_server_checker_service.h"
#include "mongo/util/concurrency/mutex.h"

namespace mongo {

    namespace {

        // Thread that runs dbHash on config servers for checking data consistency.
        boost::scoped_ptr<boost::thread> _checkerThread;

        // Protects _isConsistentFromLastCheck.
        mutex _isConsistentMutex( "ConfigServerConsistent" );
        bool _isConsistentFromLastCheck = true;

        void checkConfigConsistency() {
            while ( !inShutdown() ) {
                bool isConsistent = configServer.ok( true );

                {
                    scoped_lock sl( _isConsistentMutex );
                    _isConsistentFromLastCheck = isConsistent;
                }

                sleepsecs( 60 );
            }
        }
    }

    bool isConfigServerConsistent() {
        scoped_lock sl( _isConsistentMutex );
        return _isConsistentFromLastCheck;
    }

    bool startConfigServerChecker() {
        if ( _checkerThread == NULL ) {
            _checkerThread.reset( new boost::thread( checkConfigConsistency ));
        }

        return _checkerThread != NULL;
    }
}

