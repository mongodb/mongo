// oplog.h - writing to and reading from oplog

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

/*

     local.oplog.$main is the default
*/

#pragma once

#include "pdfile.h"
#include "db.h"
#include "dbhelpers.h"
#include "clientcursor.h"
#include "mongo/db/repl/optime.h"
#include "../util/timer.h"

namespace mongo {


    // These functions redefine the function for logOp(),
    // for either master/slave or replica sets.
    void oldRepl();  // master-slave
    void newRepl();  // replica set starting up 
    void newReplUp();// replica set after startup

    void createOplog();

    void _logOpObjRS(const BSONObj& op);

    const char rsoplog[] = "local.oplog.rs";

    /** Write operation to the log (local.oplog.$main)

       @param opstr
        "i" insert
        "u" update
        "d" delete
        "c" db cmd
        "n" no-op
        "db" declares presence of a database (ns is set to the db name + '.')

       See _logOp() in oplog.cpp for more details.
    */
    void logOp( const char *opstr, const char *ns, const BSONObj& obj, BSONObj *patt = 0, bool *b = 0, bool fromMigrate = false );

    void logKeepalive();

    /** puts obj in the oplog as a comment (a no-op).  Just for diags.
        convention is
          { msg : "text", ... }
    */
    void logOpComment(const BSONObj& obj);

    void oplogCheckCloseDatabase( Database * db );

    /**
     * take an op and apply locally
     * used for applying from an oplog
     * @param fromRepl really from replication or for testing/internal/command/etc...
     * Returns if the op was an update that could not be applied (true on failure)
     */
    bool applyOperation_inlock(const BSONObj& op, bool fromRepl = true, bool convertUpdateToUpsert = false);
}
