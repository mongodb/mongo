// ttl.cpp

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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndex

#include "mongo/platform/basic.h"

#include "mongo/db/ttl.h"

#include "mongo/base/counter.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/fsync.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/ttl_collection_cache.h"
#include "mongo/util/background.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"

namespace mongo {

using std::set;
using std::endl;
using std::list;
using std::string;
using std::vector;
using std::unique_ptr;

Counter64 ttlPasses;
Counter64 ttlDeletedDocuments;

ServerStatusMetricField<Counter64> ttlPassesDisplay("ttl.passes", &ttlPasses);
ServerStatusMetricField<Counter64> ttlDeletedDocumentsDisplay("ttl.deletedDocuments",
                                                              &ttlDeletedDocuments);

MONGO_EXPORT_SERVER_PARAMETER(ttlMonitorEnabled, bool, true);
MONGO_EXPORT_SERVER_PARAMETER(ttlMonitorSleepSecs, int, 60);  // used for testing

class TTLMonitor : public BackgroundJob {
public:
    TTLMonitor() {}
    virtual ~TTLMonitor() {}

    virtual string name() const {
        return "TTLMonitor";
    }

    static string secondsExpireField;

    virtual void run() {
        Client::initThread(name().c_str());
        AuthorizationSession::get(cc())->grantInternalAuthorization();

        while (!inShutdown()) {
            sleepsecs(ttlMonitorSleepSecs);

            LOG(3) << "TTLMonitor thread awake" << endl;

            if (!ttlMonitorEnabled) {
                LOG(1) << "TTLMonitor is disabled" << endl;
                continue;
            }

            if (lockedForWriting()) {
                // note: this is not perfect as you can go into fsync+lock between
                // this and actually doing the delete later
                LOG(3) << " locked for writing" << endl;
                continue;
            }

            try {
                doTTLPass();
            } catch (const WriteConflictException& e) {
                LOG(1) << "Got WriteConflictException in TTL thread";
            }
        }
    }

private:
    void doTTLPass() {
        // Count it as active from the moment the TTL thread wakes up
        const ServiceContext::UniqueOperationContext txnPtr = cc().makeOperationContext();
        OperationContext& txn = *txnPtr;

        // if part of replSet but not in a readable state (e.g. during initial sync), skip.
        if (repl::getGlobalReplicationCoordinator()->getReplicationMode() ==
                repl::ReplicationCoordinator::modeReplSet &&
            !repl::getGlobalReplicationCoordinator()->getMemberState().readable())
            return;

        TTLCollectionCache& ttlCollectionCache = TTLCollectionCache::get(getGlobalServiceContext());
        std::vector<std::string> ttlCollections = ttlCollectionCache.getCollections();
        std::vector<BSONObj> ttlIndexes;

        ttlPasses.increment();

        // Get all TTL indexes from every collection.
        for (const std::string& collectionNS : ttlCollections) {
            NamespaceString collectionNSS(collectionNS);
            AutoGetCollection autoGetCollection(&txn, collectionNSS, MODE_IS);
            Collection* coll = autoGetCollection.getCollection();
            if (!coll) {
                // Skip since collection has been dropped.
                continue;
            }

            CollectionCatalogEntry* collEntry = coll->getCatalogEntry();
            std::vector<std::string> indexNames;
            collEntry->getAllIndexes(&txn, &indexNames);
            for (const std::string& name : indexNames) {
                BSONObj spec = collEntry->getIndexSpec(&txn, name);
                if (spec.hasField(secondsExpireField)) {
                    ttlIndexes.push_back(spec.getOwned());
                }
            }
        }

        for (const BSONObj& idx : ttlIndexes) {
            try {
                doTTLForIndex(&txn, idx);
            } catch (const DBException& dbex) {
                error() << "Error processing ttl index: " << idx << " -- " << dbex.toString();
                // Continue on to the next index.
                continue;
            }
        }
    }

    /**
     * Remove documents from the collection using the specified TTL index
     * after a sufficient amount of time has passed according to its expiry
     * specification.
     */
    void doTTLForIndex(OperationContext* txn, BSONObj idx) {
        const NamespaceString collectionNSS(idx["ns"].String());
        if (!userAllowedWriteNS(collectionNSS).isOK()) {
            error() << "namespace '" << collectionNSS
                    << "' doesn't allow deletes, skipping ttl job for: " << idx;
            return;
        }

        const BSONObj key = idx["key"].Obj();
        if (key.nFields() != 1) {
            error() << "key for ttl index can only have 1 field, skipping ttl job for: " << idx;
            return;
        }

        LOG(1) << "TTL -- ns: " << collectionNSS << " key: " << key;

        AutoGetCollection autoGetCollection(txn, collectionNSS, MODE_IX);
        Collection* collection = autoGetCollection.getCollection();
        if (!collection) {
            // Collection was dropped.
            return;
        }

        if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(collectionNSS)) {
            // We've stepped down since we started this function, so we should stop working
            // as we only do deletes on the primary.
            return;
        }

        IndexDescriptor* desc = collection->getIndexCatalog()->findIndexByKeyPattern(txn, key);
        if (!desc) {
            LOG(1) << "index not found (index build in progress? index dropped?), skipping "
                   << "ttl job for: " << idx;
            return;
        }

        // Re-read 'idx' from the descriptor, in case the collection or index definition
        // changed before we re-acquired the collection lock.
        idx = desc->infoObj();

        if (IndexType::INDEX_BTREE != IndexNames::nameToType(desc->getAccessMethodName())) {
            error() << "special index can't be used as a ttl index, skipping ttl job for: " << idx;
            return;
        }

        BSONElement secondsExpireElt = idx[secondsExpireField];
        if (!secondsExpireElt.isNumber()) {
            error() << "ttl indexes require the " << secondsExpireField << " field to be "
                    << "numeric but received a type of " << typeName(secondsExpireElt.type())
                    << ", skipping ttl job for: " << idx;
            return;
        }

        const Date_t kDawnOfTime =
            Date_t::fromMillisSinceEpoch(std::numeric_limits<long long>::min());
        const Date_t expirationTime = Date_t::now() - Seconds(secondsExpireElt.numberLong());
        const BSONObj startKey = BSON("" << kDawnOfTime);
        const BSONObj endKey = BSON("" << expirationTime);
        const bool endKeyInclusive = true;
        // The canonical check as to whether a key pattern element is "ascending" or
        // "descending" is (elt.number() >= 0).  This is defined by the Ordering class.
        const InternalPlanner::Direction direction = (key.firstElement().number() >= 0)
            ? InternalPlanner::Direction::FORWARD
            : InternalPlanner::Direction::BACKWARD;

        // We need to pass into the DeleteStageParams (below) a CanonicalQuery with a BSONObj that
        // queries for the expired documents correctly so that we do not delete documents that are
        // not actually expired when our snapshot changes during deletion.
        const char* keyFieldName = key.firstElement().fieldName();
        BSONObj query =
            BSON(keyFieldName << BSON("$gte" << kDawnOfTime << "$lte" << expirationTime));
        auto qr = stdx::make_unique<QueryRequest>(collectionNSS);
        qr->setFilter(query);
        auto canonicalQuery = CanonicalQuery::canonicalize(
            txn, std::move(qr), ExtensionsCallbackDisallowExtensions());
        invariantOK(canonicalQuery.getStatus());

        DeleteStageParams params;
        params.isMulti = true;
        params.canonicalQuery = canonicalQuery.getValue().get();

        unique_ptr<PlanExecutor> exec =
            InternalPlanner::deleteWithIndexScan(txn,
                                                 collection,
                                                 params,
                                                 desc,
                                                 startKey,
                                                 endKey,
                                                 endKeyInclusive,
                                                 PlanExecutor::YIELD_AUTO,
                                                 direction);

        Status result = exec->executePlan();
        if (!result.isOK()) {
            error() << "ttl query execution for index " << idx
                    << " failed with status: " << redact(result);
            return;
        }

        const long long numDeleted = DeleteStage::getNumDeleted(*exec);
        ttlDeletedDocuments.increment(numDeleted);
        LOG(1) << "\tTTL deleted: " << numDeleted << endl;
    }
};

namespace {
// The global TTLMonitor object is intentionally leaked.  Even though it is only used in one
// function, we declare it here to indicate to the leak sanitizer that the leak of this object
// should not be reported.
TTLMonitor* ttlMonitor = nullptr;
}  // namespace

void startTTLBackgroundJob() {
    ttlMonitor = new TTLMonitor();
    ttlMonitor->go();
}

string TTLMonitor::secondsExpireField = "expireAfterSeconds";
}
