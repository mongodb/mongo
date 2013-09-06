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

#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/commands.h"
#include "mongo/db/field_parser.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/range_deleter_service.h"
#include "mongo/s/collection_metadata.h"
#include "mongo/s/d_logic.h"
#include "mongo/s/range_arithmetic.h"

namespace mongo {

    using mongoutils::str::stream;

    enum CleanupResult {
        CleanupResult_Done, CleanupResult_Continue, CleanupResult_Error
    };

    /**
     * Cleans up one range of orphaned data starting from a range that overlaps or starts at
     * 'startingFromKey'.  If empty, startingFromKey is the minimum key of the sharded range.
     *
     * @return CleanupResult_Continue and 'stoppedAtKey' if orphaned range was found and cleaned
     * @return CleanupResult_Done if no orphaned ranges remain
     * @return CleanupResult_Error and 'errMsg' if an error occurred
     *
     * If the collection is not sharded, returns CleanupResult_Done.
     */
    CleanupResult cleanupOrphanedData( const NamespaceString& ns,
                                       const BSONObj& startingFromKeyConst,
                                       bool secondaryThrottle,
                                       BSONObj* stoppedAtKey,
                                       string* errMsg ) {

        BSONObj startingFromKey = startingFromKeyConst;

        CollectionMetadataPtr metadata = shardingState.getCollectionMetadata( ns.toString() );
        if ( !metadata || metadata->getKeyPattern().isEmpty() ) {

            warning() << "skipping orphaned data cleanup for " << ns.toString()
                      << ", collection is not sharded" << endl;

            return CleanupResult_Done;
        }

        BSONObj keyPattern = metadata->getKeyPattern();
        if ( !startingFromKey.isEmpty() ) {
            if ( startingFromKey.nFields() != keyPattern.nFields() ) {

                *errMsg = stream() << "could not cleanup orphaned data, start key "
                                   << startingFromKey
                                   << " does not match shard key pattern " << keyPattern;

                warning() << *errMsg << endl;
                return CleanupResult_Error;
            }
        }
        else {
            startingFromKey = metadata->getMinKey();
        }

        KeyRange orphanRange;
        if ( !metadata->getNextOrphanRange( startingFromKey, &orphanRange ) ) {

            LOG( 1 ) << "orphaned data cleanup requested for " << ns.toString()
                     << " starting from " << startingFromKey
                     << ", no orphan ranges remain" << endl;

            return CleanupResult_Done;
        }
        *stoppedAtKey = orphanRange.maxKey;

        // We're done with this metadata now, no matter what happens
        metadata.reset();

        LOG( 1 ) << "orphaned data cleanup requested for " << ns.toString()
                 << " starting from " << startingFromKey
                 << ", removing next orphan range"
                 << " [" << orphanRange.minKey << "," << orphanRange.maxKey << ")"
                 << endl;

        // Metadata snapshot may be stale now, but deleter checks metadata again in write lock
        // before delete.
        if ( !getDeleter()->deleteNow( ns.toString(),
                                       orphanRange.minKey,
                                       orphanRange.maxKey,
                                       keyPattern,
                                       secondaryThrottle,
                                       errMsg ) ) {

            warning() << *errMsg << endl;
            return CleanupResult_Error;
        }

        return CleanupResult_Continue;
    }

    /**
     * Cleanup orphaned data command.  Called on a particular namespace, and if the collection
     * is sharded will clean up a single orphaned data range which overlaps or starts after a
     * passed-in 'startingFromKey'.  Returns true and a 'stoppedAtKey' (which will start a
     * search for the next orphaned range if the command is called again) or no key if there
     * are no more orphaned ranges in the collection.
     *
     * If the collection is not sharded, returns true but no 'stoppedAtKey'.
     * On failure, returns false and an error message.
     *
     * Calling this command repeatedly until no 'stoppedAtKey' is returned ensures that the
     * full collection range is searched for orphaned documents, but since sharding state may
     * change between calls there is no guarantee that all orphaned documents were found unless
     * the balancer is off.
     *
     * Safe to call with the balancer on.
     */
    class CleanupOrphanedCommand : public Command {
    public:
        CleanupOrphanedCommand() :
                Command( "cleanupOrphaned" ) {}

        virtual bool slaveOk() const { return false; }
        virtual bool adminOnly() const { return true; }
        virtual bool localHostOnlyIfNoAuth( const BSONObj& cmdObj ) { return false; }

        virtual Status checkAuthForCommand( ClientBasic* client,
                                            const std::string& dbname,
                                            const BSONObj& cmdObj ) {
            return client->getAuthorizationSession()->checkAuthForPrivilege(
                Privilege( AuthorizationManager::CLUSTER_RESOURCE_NAME,
                           ActionType::cleanupOrphaned ) );
        }

        virtual LockType locktype() const { return NONE; }

        // Input
        static BSONField<string> nsField;
        static BSONField<BSONObj> startingFromKeyField;
        static BSONField<bool> secondaryThrottleField;

        // Output
        static BSONField<BSONObj> stoppedAtKeyField;

        bool run( string const &db,
                  BSONObj &cmdObj,
                  int,
                  string &errmsg,
                  BSONObjBuilder &result,
                  bool ) {

            string ns;
            if ( !FieldParser::extract( cmdObj, nsField, &ns, &errmsg ) ) {
                return false;
            }

            if ( ns == "" ) {
                errmsg = "no collection name specified";
                return false;
            }

            BSONObj startingFromKey;
            if ( !FieldParser::extract( cmdObj,
                                        startingFromKeyField,
                                        &startingFromKey,
                                        &errmsg ) ) {
                return false;
            }

            bool secondaryThrottle = true;
            if ( !FieldParser::extract( cmdObj,
                                        secondaryThrottleField,
                                        &secondaryThrottle,
                                        &errmsg ) ) {
                return false;
            }

            BSONObj stoppedAtKey;
            CleanupResult cleanupResult = cleanupOrphanedData( NamespaceString( ns ),
                                                               startingFromKey,
                                                               secondaryThrottle,
                                                               &stoppedAtKey,
                                                               &errmsg );

            if ( cleanupResult == CleanupResult_Error ) {
                return false;
            }

            if ( cleanupResult == CleanupResult_Continue ) {
                result.append( stoppedAtKeyField(), stoppedAtKey );
            }
            else {
                dassert( cleanupResult == CleanupResult_Done );
            }

            return true;
        }
    };

    BSONField<string> CleanupOrphanedCommand::nsField( "cleanupOrphaned" );
    BSONField<BSONObj> CleanupOrphanedCommand::startingFromKeyField( "startingFromKey" );
    BSONField<bool> CleanupOrphanedCommand::secondaryThrottleField( "secondaryThrottleField" );
    BSONField<BSONObj> CleanupOrphanedCommand::stoppedAtKeyField( "stoppedAtKey" );

    MONGO_INITIALIZER(RegisterCleanupOrphanedCommand)(InitializerContext* context) {
        // Leaked intentionally: a Command registers itself when constructed.
        new CleanupOrphanedCommand();
        return Status::OK();
    }

} // namespace mongo

