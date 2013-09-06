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

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/database.h"
#include "mongo/db/repl/master_slave.h"  // replAllDead
#include "mongo/db/repl/replication_server_status.h"  // replSettings
#include "mongo/db/repl/rs.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    /* note we always return true for the "local" namespace.

       we should not allow most operations when not the master
       also we report not master if we are "dead".

       See also CmdIsMaster.

       If 'client' is not specified, the current client is used.
    */
    inline bool _isMaster() {
        if( replSet ) {
            if( theReplSet )
                return theReplSet->isPrimary();
            return false;
        }

        if( ! replSettings.slave )
            return true;

        if ( replAllDead )
            return false;

        if( replSettings.master ) {
            // if running with --master --slave, allow.
            return true;
        }

        //TODO: Investigate if this is needed/used, see SERVER-9188
        if ( cc().isGod() )
            return true;

        return false;
    }
    inline bool isMaster(const char * dbname = 0) {
        if( _isMaster() )
            return true;
        if ( ! dbname ) {
            Database *database = cc().database();
            verify( database );
            dbname = database->name().c_str();
        }
        return strcmp( dbname , "local" ) == 0;
    }
    inline bool isMasterNs( const char *ns ) {
        if ( _isMaster() )
            return true;
        verify( ns );
        if ( ! str::startsWith( ns , "local" ) )
            return false;
        return ns[5] == 0 || ns[5] == '.';
    }

} // namespace mongo
