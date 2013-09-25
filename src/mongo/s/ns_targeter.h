/**
 * Copyright (C) 2013 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <string>

#include "mongo/bson/bsonobj.h"
#include "mongo/base/status.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/chunk_version.h"

namespace mongo {

    struct ShardEndpoint;

    /**
     * The NSTargeter interface is used by a WriteOp to generate and target child write operations
     * to a particular collection.
     *
     * The lifecyle of a NSTargeter is:
     *   0. refreshIfNeeded() to get targeting information (this must be the *first* call to the
     *      targeter, since we may need to load initial information)
     *   1. targetDoc/targetQuery as many times as is required
     *   1a. On targeting failure, we may need to refresh, note this and goto 0.
     *   2. On stale config from a child write operation, note the error
     *   3. Goto 0.
     *
     * The refreshIfNeeded() operation must make progress against noted targeting or stale config
     * failures, see comments below.  No functions may block for shared resources or network calls
     * except refreshIfNeeded().
     *
     * Implementers are free to define more specific targeting error codes to allow more complex
     * error handling.
     *
     * Interface must be externally synchronized if used in multiple threads, for now.
     * TODO: Determine if we should internally synchronize.
     */
    class NSTargeter {
    public:

        virtual ~NSTargeter() {
        }

        /**
         * Returns the namespace targeted.
         */
        virtual const NamespaceString& getNS() const = 0;

        /**
         * Refreshes the targeting metadata for the namespace if needed, based on previously-noted
         * stale responses and targeting failures.
         *
         * After this function is called, the targeter should be in a state such that the noted
         * stale responses are not seen again and if a targeting failure occurred it reloaded -
         * it should make progress.
         *
         * NOTE: This function may block for shared resources or network calls.
         * Returns !OK with message if could not refresh
         */
        virtual Status refreshIfNeeded() = 0;

        /**
         * Returns a ShardEndpoint for a single document write.
         *
         * Returns ShardKeyNotFound if document does not have a full shard key.
         * Returns !OK with message if document could not be targeted for other reasons.
         */
        virtual Status targetDoc( const BSONObj& doc,
                                  ShardEndpoint** endpoint ) const = 0;

        /**
         * Returns a vector of ShardEndpoints for a potentially multi-shard query.
         *
         * Returns !OK with message if query could not be targeted.
         */
        virtual Status targetQuery( const BSONObj& query,
                                    std::vector<ShardEndpoint*>* endpoints ) const = 0;

        /**
         * Informs the targeter of stale config responses for this namespace from an endpoint, with
         * further information available in the returned staleInfo.
         *
         * Any stale responses noted here will be taken into account on the next refresh.
         */
        virtual void noteStaleResponse( const ShardEndpoint& endpoint,
                                        const BSONObj& staleInfo ) = 0;

        /**
         * Informs the targeter that a remote refresh is needed on the next refresh.
         */
        virtual void noteNeedsRefresh() = 0;

    };

    /**
     * A ShardEndpoint represents a destination for a targeted query or document.  It contains both
     * the logical target (shard name/version/broadcast) and the physical target (host name).
     */
    struct ShardEndpoint {

        ShardEndpoint() {
        }

        ShardEndpoint( const ShardEndpoint& other ) :
                shardName( other.shardName ),
                shardVersion( other.shardVersion ),
                shardHost( other.shardHost ) {
        }

        ShardEndpoint( const string& shardName,
                       const ChunkVersion& shardVersion,
                       const ConnectionString& shardHost ) :
                shardName( shardName ), shardVersion( shardVersion ), shardHost( shardHost ) {
        }

        const std::string shardName;
        const ChunkVersion shardVersion;
        const ConnectionString shardHost;

        //
        // For testing *only* - do not use as part of API
        //

        BSONObj toBSON() const {
            BSONObjBuilder b;
            appendBSON( &b );
            return b.obj();
        }

        void appendBSON( BSONObjBuilder* builder ) const {
            builder->append( "shardName", shardName );
            shardVersion.addToBSON( *builder, "shardVersion" );
            builder->append( "shardHost", shardHost.toString() );
        }
    };

} // namespace mongo
