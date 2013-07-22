//@file update.h

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

#include "mongo/pch.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/curop.h"
#include "mongo/db/query_plan_selection_policy.h"

namespace mongo {

    // ---------- public -------------

    struct UpdateResult {
        const bool existing; // if existing objects were modified
        const bool mod;      // was this a $ mod
        const long long num; // how many objects touched
        OID upserted;  // if something was upserted, the new _id of the object

        UpdateResult( bool e, bool m, unsigned long long n , const BSONObj& upsertedObject )
            : existing(e) , mod(m), num(n) {
            upserted.clear();
            BSONElement id = upsertedObject["_id"];
            if ( ! e && n == 1 && id.type() == jstOID ) {
                upserted = id.OID();
            }
        }
    };

    class RemoveSaver;

    /** Returns true if updates are supposed to be handle by the new update framework */
    bool isNewUpdateFrameworkEnabled();

    /** switches state from enabled/disabled; returns new state */
    bool toggleNewUpdateFrameworkEnabled();

    /* returns true if an existing object was updated, false if no existing object was found.
       multi - update multiple objects - mostly useful with things like $set
       su - allow access to system namespaces (super user)
    */
    UpdateResult updateObjects(const char* ns,
                               const BSONObj& updateobj,
                               const BSONObj& pattern,
                               bool upsert,
                               bool multi,
                               bool logop,
                               OpDebug& debug,
                               bool fromMigrate = false,
                               const QueryPlanSelectionPolicy& planPolicy = QueryPlanSelectionPolicy::any());

    /*
     * Similar to updateObjects but not strict about applying mods that can fail during initial
     * replication.
     *
     * Reference ticket: SERVER-4781
     */
    UpdateResult updateObjectsForReplication(const char* ns,
                                             const BSONObj& updateobj,
                                             const BSONObj& pattern,
                                             bool upsert,
                                             bool multi,
                                             bool logop,
                                             OpDebug& debug,
                                             bool fromMigrate = false,
                                             const QueryPlanSelectionPolicy& planPolicy =
                                                 QueryPlanSelectionPolicy::any());

    UpdateResult _updateObjects(bool su,
                                const char* ns,
                                const BSONObj& updateobj,
                                const BSONObj& pattern,
                                bool upsert,
                                bool multi,
                                bool logop,
                                OpDebug& debug,
                                RemoveSaver* rs = 0,
                                bool fromMigrate = false,
                                const QueryPlanSelectionPolicy& planPolicy
                                    = QueryPlanSelectionPolicy::any(),
                                bool forReplication = false);

    UpdateResult _updateObjectsNEW(bool su,
                                   const char* ns,
                                   const BSONObj& updateobj,
                                   const BSONObj& pattern,
                                   bool upsert,
                                   bool multi,
                                   bool logop,
                                   OpDebug& debug,
                                   RemoveSaver* rs = 0,
                                   bool fromMigrate = false,
                                   const QueryPlanSelectionPolicy& planPolicy
                                       = QueryPlanSelectionPolicy::any(),
                                   bool forReplication = false);

    /**
     * takes the from document and returns a new document
     * after apply all the operators 
     * e.g. 
     *   applyUpdateOperators( BSON( "x" << 1 ) , BSON( "$inc" << BSON( "x" << 1 ) ) );
     *   returns: { x : 2 }
     */
    BSONObj applyUpdateOperators( const BSONObj& from, const BSONObj& operators );
    
}  // namespace mongo
