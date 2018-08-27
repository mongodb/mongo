/**
 * Copyright (C) 2017 MongoDB Inc.
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
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <boost/intrusive_ptr.hpp>
#include <boost/optional.hpp>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "mongo/client/dbclient_base.h"
#include "mongo/db/collection_index_usage_tracker.h"
#include "mongo/db/generic_cursor.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/storage/backup_cursor_service.h"

namespace mongo {

class ExpressionContext;
class Pipeline;
class PipelineDeleter;

/**
 * Any functionality needed by an aggregation stage that is either context specific to a mongod or
 * mongos process, or is only compiled in to one of those two binaries must be accessed via this
 * interface. This allows all DocumentSources to be parsed on either mongos or mongod, but only
 * executable where it makes sense.
 */
class MongoProcessInterface {
public:
    enum class CurrentOpConnectionsMode { kIncludeIdle, kExcludeIdle };
    enum class CurrentOpUserMode { kIncludeAll, kExcludeOthers };
    enum class CurrentOpTruncateMode { kNoTruncation, kTruncateOps };
    enum class CurrentOpLocalOpsMode { kLocalMongosOps, kRemoteShardOps };
    enum class CurrentOpSessionsMode { kIncludeIdle, kExcludeIdle };

    struct MakePipelineOptions {
        MakePipelineOptions(){};

        bool optimize = true;
        bool attachCursorSource = true;
    };

    virtual ~MongoProcessInterface(){};

    /**
     * Sets the OperationContext of the DBDirectClient returned by directClient(). This method must
     * be called after updating the 'opCtx' member of the ExpressionContext associated with the
     * document source.
     */
    virtual void setOperationContext(OperationContext* opCtx) = 0;

    /**
     * Always returns a DBDirectClient. The return type in the function signature is a DBClientBase*
     * because DBDirectClient isn't linked into mongos.
     */
    virtual DBClientBase* directClient() = 0;

    /**
     * Note that in some rare cases this could return a false negative but will never return a false
     * positive. This method will be fixed in the future once it becomes possible to avoid false
     * negatives.
     */
    virtual bool isSharded(OperationContext* opCtx, const NamespaceString& ns) = 0;

    /**
     * Inserts 'objs' into 'ns' and throws a UserException if the insert fails.
     */
    virtual void insert(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        const NamespaceString& ns,
                        const std::vector<BSONObj>& objs) = 0;

    /**
     * Updates the documents matching 'queries' with the objects 'updates'. Throws a UserException
     * if any of the updates fail.
     */
    virtual void update(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                        const NamespaceString& ns,
                        const std::vector<BSONObj>& queries,
                        const std::vector<BSONObj>& updates,
                        bool upsert,
                        bool multi) = 0;

    virtual CollectionIndexUsageMap getIndexStats(OperationContext* opCtx,
                                                  const NamespaceString& ns) = 0;

    /**
     * Appends operation latency statistics for collection "nss" to "builder"
     */
    virtual void appendLatencyStats(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    bool includeHistograms,
                                    BSONObjBuilder* builder) const = 0;

    /**
     * Appends storage statistics for collection "nss" to "builder"
     */
    virtual Status appendStorageStats(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      const BSONObj& param,
                                      BSONObjBuilder* builder) const = 0;

    /**
     * Appends the record count for collection "nss" to "builder".
     */
    virtual Status appendRecordCount(OperationContext* opCtx,
                                     const NamespaceString& nss,
                                     BSONObjBuilder* builder) const = 0;

    /**
     * Gets the collection options for the collection given by 'nss'.
     */
    virtual BSONObj getCollectionOptions(const NamespaceString& nss) = 0;

    /**
     * Performs the given rename command if the collection given by 'targetNs' has the same options
     * as specified in 'originalCollectionOptions', and has the same indexes as 'originalIndexes'.
     *
     * Throws an exception if the collection options and/or indexes are different.
     */
    virtual void renameIfOptionsAndIndexesHaveNotChanged(
        OperationContext* opCtx,
        const BSONObj& renameCommandObj,
        const NamespaceString& targetNs,
        const BSONObj& originalCollectionOptions,
        const std::list<BSONObj>& originalIndexes) = 0;

    /**
     * Parses a Pipeline from a vector of BSONObjs representing DocumentSources. The state of the
     * returned pipeline will depend upon the supplied MakePipelineOptions:
     * - The boolean opts.optimize determines whether the pipeline will be optimized.
     * - If opts.attachCursorSource is false, the pipeline will be returned without attempting to
     *   add an initial cursor source.
     *
     * This function returns a non-OK status if parsing the pipeline failed.
     */
    virtual StatusWith<std::unique_ptr<Pipeline, PipelineDeleter>> makePipeline(
        const std::vector<BSONObj>& rawPipeline,
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const MakePipelineOptions opts = MakePipelineOptions{}) = 0;

    /**
     * Attaches a cursor source to the start of a pipeline. Performs no further optimization. This
     * function asserts if the collection to be aggregated is sharded. NamespaceNotFound will be
     * returned if ExpressionContext has a UUID and that UUID doesn't exist anymore. That should be
     * the only case where NamespaceNotFound is returned.
     */
    virtual Status attachCursorSourceToPipeline(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, Pipeline* pipeline) = 0;

    /**
     * Returns a vector of owned BSONObjs, each of which contains details of an in-progress
     * operation or, optionally, an idle connection. If userMode is kIncludeAllUsers, report
     * operations for all authenticated users; otherwise, report only the current user's operations.
     */
    virtual std::vector<BSONObj> getCurrentOps(OperationContext* opCtx,
                                               CurrentOpConnectionsMode connMode,
                                               CurrentOpSessionsMode sessionMode,
                                               CurrentOpUserMode userMode,
                                               CurrentOpTruncateMode) const = 0;

    /**
     * Returns the name of the local shard if sharding is enabled, or an empty string.
     */
    virtual std::string getShardName(OperationContext* opCtx) const = 0;

    /**
     * Returns the fields of the document key (in order) for the collection corresponding to 'uuid',
     * including the shard key and _id. If _id is not in the shard key, it is added last. If the
     * collection is not sharded or no longer exists, returns only _id. Also retrurns a boolean that
     * indicates whether the returned fields of the document key are final and will never change for
     * the given collection, either because the collection was dropped or has become sharded.
     */
    virtual std::pair<std::vector<FieldPath>, bool> collectDocumentKeyFields(
        OperationContext* opCtx, NamespaceStringOrUUID nssOrUUID) const = 0;

    /**
     * Returns zero or one documents with the document key 'documentKey'. 'documentKey' is treated
     * as a unique identifier of a document, and may include an _id or all fields from the shard key
     * and an _id. Throws if more than one match was found. Returns boost::none if no matching
     * documents were found, including cases where the given namespace does not exist.
     */
    virtual boost::optional<Document> lookupSingleDocument(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const NamespaceString& nss,
        UUID,
        const Document& documentKey,
        boost::optional<BSONObj> readConcern) = 0;

    /**
     * Returns a vector of all local cursors.
     */
    virtual std::vector<GenericCursor> getCursors(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) const = 0;

    /**
     * The following methods forward to the BackupCursorService decorating the ServiceContext.
     */
    virtual void fsyncLock(OperationContext* opCtx) = 0;

    virtual void fsyncUnlock(OperationContext* opCtx) = 0;

    virtual BackupCursorState openBackupCursor(OperationContext* opCtx) = 0;

    virtual void closeBackupCursor(OperationContext* opCtx, std::uint64_t cursorId) = 0;

    /**
     * Returns a vector of BSON objects, where each entry in the vector describes a plan cache entry
     * inside the cache for the given namespace. Only those entries which match the supplied
     * MatchExpression are returned.
     */
    virtual std::vector<BSONObj> getMatchingPlanCacheEntryStats(OperationContext*,
                                                                const NamespaceString&,
                                                                const MatchExpression*) const = 0;

    /**
     * Returns true if there is an index on 'nss' with properties that will guarantee that a
     * document with non-array values for each of 'uniqueKeyPaths' will have at most one matching
     * document in 'nss'.
     *
     * Specifically, such an index must include all the fields, be unique, not be a partial index,
     * and match the operation's collation as given by 'expCtx'.
     */
    virtual bool uniqueKeyIsSupportedByIndex(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                             const NamespaceString& nss,
                                             const std::set<FieldPath>& uniqueKeyPaths) const = 0;
};

}  // namespace mongo
