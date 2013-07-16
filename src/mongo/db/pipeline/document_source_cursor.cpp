/**
 * Copyright 2011 (c) 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "mongo/pch.h"

#include "mongo/db/pipeline/document_source.h"

#include "mongo/db/clientcursor.h"
#include "mongo/db/instance.h"
#include "mongo/db/ops/query.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/s/d_logic.h"
#include "mongo/s/stale_exception.h" // for SendStaleConfigException

namespace mongo {

    DocumentSourceCursor::~DocumentSourceCursor() {
        dispose();
    }

    bool DocumentSourceCursor::eof() {
        /* if we haven't gotten the first one yet, do so now */
        if (unstarted)
            loadBatch();

        return _currentBatch.empty();
    }

    bool DocumentSourceCursor::advance() {
        DocumentSource::advance(); // check for interrupts

        /* if we haven't gotten the first one yet, do so now */
        if (unstarted)
            loadBatch();

        verify(!_currentBatch.empty());
        _currentBatch.pop_front();

        if (_currentBatch.empty())
            loadBatch();

        return !_currentBatch.empty();
    }

    Document DocumentSourceCursor::getCurrent() {
        verify(!_currentBatch.empty());
        return _currentBatch.front();
    }

    void DocumentSourceCursor::dispose() {
        if (_cursorId) {
            ClientCursor::erase(_cursorId);
            _cursorId = 0;
        }

        _collMetadata.reset();
        _currentBatch.clear();
    }

    bool DocumentSourceCursor::canUseCoveredIndex(ClientCursor* cursor) {
        // We can't use a covered index when we have collection metadata because we
        // need to examine the object to see if it belongs on this shard
        return (!_collMetadata &&
                cursor->ok() && cursor->c()->keyFieldsOnly());
    }

    void DocumentSourceCursor::yieldSometimes(ClientCursor* cursor) {
        try { // SERVER-5752 may make this try unnecessary
            // if we are index only we don't need the recored
            bool cursorOk = cursor->yieldSometimes(canUseCoveredIndex(cursor)
                                                     ? ClientCursor::DontNeed
                                                     : ClientCursor::WillNeed);
            uassert( 16028, "collection or database disappeared when cursor yielded", cursorOk );
        }
        catch(SendStaleConfigException& e){
            // We want to ignore this because the migrated documents will be filtered out of the
            // cursor anyway and, we don't want to restart the aggregation after every migration.

            log() << "Config changed during aggregation - command will resume" << endl;
            // useful for debugging but off by default to avoid looking like a scary error.
            LOG(1) << "aggregation stale config exception: " << e.what() << endl;
        }
    }

    void DocumentSourceCursor::loadBatch() {
        unstarted = false;

        if (!_cursorId) {
            dispose();
            return;
        }

        // We have already validated the sharding version when we constructed the cursor
        // so we shouldn't check it again.
        Lock::DBRead lk(ns);
        Client::Context ctx(ns, dbpath, /*doVersion=*/false);

        ClientCursor::Pin pin(_cursorId);
        ClientCursor* cursor = pin.c();

        uassert(16950, "Cursor deleted. Was the collection or database dropped?",
                cursor);

        cursor->c()->recoverFromYield();

        int memUsageBytes = 0;
        for( ; cursor->ok(); cursor->advance() ) {

            yieldSometimes(cursor);
            if ( !cursor->ok() ) {
                // The cursor was exhausted during the yield.
                break;
            }

            if ( !cursor->currentMatches() || cursor->currentIsDup() )
                continue;

            // grab the matching document
            if (canUseCoveredIndex(cursor)) {
                // Can't have collection metadata if we are here
                BSONObj indexKey = cursor->currKey();
                _currentBatch.push_back(Document(cursor->c()->keyFieldsOnly()->hydrate(indexKey)));
            }
            else {
                BSONObj next = cursor->current();

                // check to see if this is a new object we don't own yet
                // because of a chunk migration
                if (_collMetadata) {
                    KeyPattern kp( _collMetadata->getKeyPattern() );
                    if ( !_collMetadata->keyBelongsToMe( kp.extractSingleKey( next ) ) ) continue;
                }

                _currentBatch.push_back(_projection
                                            ? documentFromBsonWithDeps(next, _dependencies)
                                            : Document(next));
            }

            memUsageBytes += _currentBatch.back().getApproximateSize();

            if (memUsageBytes > MaxBytesToReturnToClientAtOnce) {
                // End this batch and prepare cursor for yielding.
                cursor->advance();

                if (cursor->c()->supportYields()) {
                    ClientCursor::YieldData data;
                    cursor->prepareToYield(data);
                } else {
                    cursor->c()->noteLocation();
                }

                return;
            }
        }

        // If we got here, there aren't any more documents.
        // The Cursor must be released, see SERVER-6123.
        pin.release();
        ClientCursor::erase(_cursorId);
        _cursorId = 0;
        _collMetadata.reset();
    }

    void DocumentSourceCursor::setSource(DocumentSource *pSource) {
        /* this doesn't take a source */
        verify(false);
    }

    void DocumentSourceCursor::sourceToBson(
        BSONObjBuilder *pBuilder, bool explain) const {

        /* this has no analog in the BSON world, so only allow it for explain */
        if (explain)
        {
            BSONObj bsonObj;
            
            pBuilder->append("query", _query);

            if (!_sort.isEmpty()) {
                pBuilder->append("sort", _sort);
            }

            BSONObj projectionSpec;
            if (_projection) {
                projectionSpec = _projection->getSpec();
                pBuilder->append("projection", projectionSpec);
            }

            // construct query for explain
            BSONObjBuilder queryBuilder;
            queryBuilder.append("$query", _query);
            if (!_sort.isEmpty())
                queryBuilder.append("$orderby", _sort);
            queryBuilder.append("$explain", 1);
            Query query(queryBuilder.obj());

            DBDirectClient directClient;
            BSONObj explainResult(directClient.findOne(ns, query, _projection
                                                                  ? &projectionSpec
                                                                  : NULL));

            pBuilder->append("cursor", explainResult);
        }
    }

    DocumentSourceCursor::DocumentSourceCursor(const string& ns,
                                               CursorId cursorId,
                                               const intrusive_ptr<ExpressionContext> &pCtx)
        : DocumentSource(pCtx)
        , unstarted(true)
        , ns(ns)
        , _cursorId(cursorId)
        , _collMetadata(shardingState.needCollectionMetadata( ns )
                        ? shardingState.getCollectionMetadata( ns )
                        : CollectionMetadataPtr())
    {}

    intrusive_ptr<DocumentSourceCursor> DocumentSourceCursor::create(
            const string& ns,
            CursorId cursorId,
            const intrusive_ptr<ExpressionContext> &pExpCtx) {
        return new DocumentSourceCursor(ns, cursorId, pExpCtx);
    }

    void DocumentSourceCursor::setProjection(const BSONObj& projection, const ParsedDeps& deps) {
        verify(!_projection);
        _projection.reset(new Projection);
        _projection->init(projection);

        ClientCursor::Pin pin (_cursorId);
        verify(pin.c());
        pin.c()->fields = _projection;

        _dependencies = deps;
    }
}
