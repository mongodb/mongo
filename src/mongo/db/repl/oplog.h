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

#pragma once

namespace mongo {

    class BSONObj;
    class Database;

    // These functions redefine the function for logOp(),
    // for either master/slave or replica sets.
    void oldRepl();  // master-slave
    void newRepl();  // replica set starting up 
    void newReplUp();// replica set after startup

    // Create a new capped collection for the oplog if it doesn't yet exist.
    // This will be either local.oplog.rs (replica sets) or local.oplog.$main (master/slave)
    // If the collection already exists, set the 'last' OpTime if master/slave (side effect!)
    void createOplog();

    // This poorly-named function writes an op into the replica-set oplog;
    // used internally by replication secondaries after they have applied an op
    void _logOpObjRS(const BSONObj& op);

    const char rsoplog[] = "local.oplog.rs";

    /** Log an operation to the local oplog 

       @param opstr
        "i" insert
        "u" update
        "d" delete
        "c" db cmd
        "n" no-op
        "db" declares presence of a database (ns is set to the db name + '.')

       For 'u' records, 'obj' captures the mutation made to the object but not
       the object itself. In that case, we provide also 'fullObj' which is the
       image of the object _after_ the mutation logged here was applied.

       See _logOp() in oplog.cpp for more details.
    */
    void logOp( const char *opstr, const char *ns, const BSONObj& obj,
                BSONObj *patt = NULL, bool *b = NULL, bool fromMigrate = false,
                const BSONObj* fullObj = NULL );

    // Log an empty no-op operation to the local oplog
    void logKeepalive();

    /** puts obj in the oplog as a comment (a no-op).  Just for diags.
        convention is
          { msg : "text", ... }
    */
    void logOpComment(const BSONObj& obj);

    // Flush out the cached pointers to the local database and oplog.
    // Used by the closeDatabase command to ensure we don't cache closed things.
    void oplogCheckCloseDatabase( Database * db );

    /**
     * take an op and apply locally
     * used for applying from an oplog
     * @param fromRepl really from replication or for testing/internal/command/etc...
     * @param convertUpdateToUpsert convert some updates to upserts for idempotency reasons
     * Returns if the op was an update that could not be applied (true on failure)
     */
    bool applyOperation_inlock(const BSONObj& op, 
                               bool fromRepl = true, 
                               bool convertUpdateToUpsert = false);
}
