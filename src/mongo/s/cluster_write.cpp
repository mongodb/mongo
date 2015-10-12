/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/cluster_write.h"

#include <string>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/catalog/catalog_cache.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_manager_targeter.h"
#include "mongo/s/client/dbclient_multi_command.h"
#include "mongo/s/config.h"
#include "mongo/s/dbclient_shard_resolver.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batch_write_exec.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::shared_ptr;
using std::unique_ptr;
using std::vector;
using std::map;
using std::string;
using std::stringstream;

const int ConfigOpTimeoutMillis = 30 * 1000;

namespace {

/**
 * Constructs the BSON specification document for the given namespace, index key
 * and options.
 */
BSONObj createIndexDoc(const string& ns, const BSONObj& keys, bool unique) {
    BSONObjBuilder indexDoc;
    indexDoc.append("ns", ns);
    indexDoc.append("key", keys);

    stringstream indexName;

    bool isFirstKey = true;
    for (BSONObjIterator keyIter(keys); keyIter.more();) {
        BSONElement currentKey = keyIter.next();

        if (isFirstKey) {
            isFirstKey = false;
        } else {
            indexName << "_";
        }

        indexName << currentKey.fieldName() << "_";
        if (currentKey.isNumber()) {
            indexName << currentKey.numberInt();
        } else {
            indexName << currentKey.str();  // this should match up with shell command
        }
    }

    indexDoc.append("name", indexName.str());

    if (unique) {
        indexDoc.appendBool("unique", unique);
    }

    return indexDoc.obj();
}

void toBatchError(const Status& status, BatchedCommandResponse* response) {
    response->clear();
    response->setErrCode(status.code());
    response->setErrMessage(status.reason());
    response->setOk(false);
    dassert(response->isValid(NULL));
}

/**
 * Splits the chunks touched based from the targeter stats if needed.
 */
void splitIfNeeded(OperationContext* txn, const NamespaceString& nss, const TargeterStats& stats) {
    if (!Chunk::ShouldAutoSplit) {
        return;
    }

    auto status = grid.catalogCache()->getDatabase(txn, nss.db().toString());
    if (!status.isOK()) {
        warning() << "failed to get database config for " << nss
                  << " while checking for auto-split: " << status.getStatus();
        return;
    }

    shared_ptr<DBConfig> config = status.getValue();

    ChunkManagerPtr chunkManager;
    shared_ptr<Shard> dummyShard;
    config->getChunkManagerOrPrimary(txn, nss.ns(), chunkManager, dummyShard);

    if (!chunkManager) {
        return;
    }

    for (map<BSONObj, int>::const_iterator it = stats.chunkSizeDelta.begin();
         it != stats.chunkSizeDelta.end();
         ++it) {
        ChunkPtr chunk;
        try {
            chunk = chunkManager->findIntersectingChunk(txn, it->first);
        } catch (const AssertionException& ex) {
            warning() << "could not find chunk while checking for auto-split: " << causedBy(ex);
            return;
        }

        chunk->splitIfShould(txn, it->second);
    }
}

}  // namespace

Status clusterCreateIndex(OperationContext* txn,
                          const string& ns,
                          BSONObj keys,
                          bool unique,
                          BatchedCommandResponse* response) {
    const NamespaceString nss(ns);
    const std::string dbName = nss.db().toString();

    BSONObj indexDoc = createIndexDoc(ns, keys, unique);

    // Go through the shard insert path
    std::unique_ptr<BatchedInsertRequest> insert(new BatchedInsertRequest());
    insert->addToDocuments(indexDoc);

    BatchedCommandRequest request(insert.release());
    request.setNS(NamespaceString(nss.getSystemIndexesCollection()));
    request.setWriteConcern(WriteConcernOptions::Acknowledged);

    BatchedCommandResponse dummyResponse;
    if (response == NULL) {
        response = &dummyResponse;
    }

    ClusterWriter writer(false, 0);
    writer.write(txn, request, response);

    if (response->getOk() != 1) {
        return Status(static_cast<ErrorCodes::Error>(response->getErrCode()),
                      response->getErrMessage());
    }

    if (response->isErrDetailsSet()) {
        const WriteErrorDetail* errDetail = response->getErrDetails().front();

        return Status(static_cast<ErrorCodes::Error>(errDetail->getErrCode()),
                      errDetail->getErrMessage());
    }

    if (response->isWriteConcernErrorSet()) {
        const WCErrorDetail* errDetail = response->getWriteConcernError();

        return Status(static_cast<ErrorCodes::Error>(errDetail->getErrCode()),
                      errDetail->getErrMessage());
    }

    return Status::OK();
}


void ClusterWriter::write(OperationContext* txn,
                          const BatchedCommandRequest& origRequest,
                          BatchedCommandResponse* response) {
    // Add _ids to insert request if req'd
    unique_ptr<BatchedCommandRequest> idRequest(BatchedCommandRequest::cloneWithIds(origRequest));
    const BatchedCommandRequest* request = NULL != idRequest.get() ? idRequest.get() : &origRequest;

    const NamespaceString& nss = request->getNS();
    if (!nss.isValid()) {
        toBatchError(Status(ErrorCodes::InvalidNamespace, nss.ns() + " is not a valid namespace"),
                     response);
        return;
    }

    if (!NamespaceString::validCollectionName(nss.coll())) {
        toBatchError(
            Status(ErrorCodes::BadValue, str::stream() << "invalid collection name " << nss.coll()),
            response);
        return;
    }

    if (request->sizeWriteOps() == 0u) {
        toBatchError(Status(ErrorCodes::InvalidLength, "no write ops were included in the batch"),
                     response);
        return;
    }

    if (request->sizeWriteOps() > BatchedCommandRequest::kMaxWriteBatchSize) {
        toBatchError(Status(ErrorCodes::InvalidLength,
                            str::stream() << "exceeded maximum write batch size of "
                                          << BatchedCommandRequest::kMaxWriteBatchSize),
                     response);
        return;
    }

    string errMsg;
    if (request->isInsertIndexRequest() && !request->isValidIndexRequest(&errMsg)) {
        toBatchError(Status(ErrorCodes::InvalidOptions, errMsg), response);
        return;
    }

    // Config writes and shard writes are done differently
    const string dbName = nss.db().toString();

    unique_ptr<BatchedCommandRequest> requestWithWriteConcern;
    if (dbName == "config" || dbName == "admin") {
        // w:majority is the only valid write concern for writes to the config servers.
        // We also allow w:1 to come in on a user-initiated write, though we convert it here to
        // w:majority before sending it to the config servers.
        bool rewriteCmdWithWriteConcern = false;
        WriteConcernOptions writeConcern;
        if (request->isWriteConcernSet()) {
            Status status = writeConcern.parse(request->getWriteConcern());
            if (!status.isOK()) {
                toBatchError(status, response);
                return;
            }
            if (!writeConcern.validForConfigServers()) {
                toBatchError(Status(ErrorCodes::InvalidOptions,
                                    "Invalid replication write concern.  Writes to config servers "
                                    "must use w:'majority'"),
                             response);
                return;
            }
            if (writeConcern.wMode == "") {
                invariant(writeConcern.wNumNodes == 1);
                rewriteCmdWithWriteConcern = true;
            }
        } else {
            rewriteCmdWithWriteConcern = true;
        }

        if (rewriteCmdWithWriteConcern) {
            requestWithWriteConcern.reset(new BatchedCommandRequest(request->getBatchType()));
            request->cloneTo(requestWithWriteConcern.get());
            writeConcern.wMode = WriteConcernOptions::kMajority;
            writeConcern.wNumNodes = 0;
            requestWithWriteConcern->setWriteConcern(writeConcern.toBSON());
            request = requestWithWriteConcern.get();
        }

        grid.catalogManager(txn)->writeConfigServerDirect(txn, *request, response);
    } else {
        ChunkManagerTargeter targeter(request->getTargetingNSS());
        Status targetInitStatus = targeter.init(txn);

        if (!targetInitStatus.isOK()) {
            // Errors will be reported in response if we are unable to target
            warning() << "could not initialize targeter for"
                      << (request->isInsertIndexRequest() ? " index" : "")
                      << " write op in collection " << request->getTargetingNS();
        }

        DBClientShardResolver resolver;
        DBClientMultiCommand dispatcher;
        BatchWriteExec exec(&targeter, &resolver, &dispatcher);
        exec.executeBatch(txn, *request, response);

        if (_autoSplit) {
            splitIfNeeded(txn, request->getNS(), *targeter.getStats());
        }

        _stats->setShardStats(exec.releaseStats());
    }
}

ClusterWriter::ClusterWriter(bool autoSplit, int timeoutMillis)
    : _autoSplit(autoSplit), _timeoutMillis(timeoutMillis), _stats(new ClusterWriterStats) {}

const ClusterWriterStats& ClusterWriter::getStats() {
    return *_stats;
}

void ClusterWriterStats::setShardStats(BatchWriteExecStats* shardStats) {
    _shardStats.reset(shardStats);
}

bool ClusterWriterStats::hasShardStats() const {
    return NULL != _shardStats.get();
}

const BatchWriteExecStats& ClusterWriterStats::getShardStats() const {
    return *_shardStats;
}

}  // namespace mongo
