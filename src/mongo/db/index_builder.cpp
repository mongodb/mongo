/**
 *    Copyright (C) 2012 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kIndexing

#include "mongo/platform/basic.h"

#include "mongo/db/index_builder.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    AtomicUInt32 IndexBuilder::_indexBuildCount;

    IndexBuilder::IndexBuilder(const BSONObj& index) :
        BackgroundJob(true /* self-delete */), _index(index.getOwned()),
        _name(str::stream() << "repl index builder " << _indexBuildCount.addAndFetch(1)) {
    }

    IndexBuilder::~IndexBuilder() {}

    std::string IndexBuilder::name() const {
        return _name;
    }

    void IndexBuilder::run() {
        LOG(2) << "IndexBuilder building index " << _index;

        OperationContextImpl txn;

        Client::initThread(name().c_str());
        Lock::ParallelBatchWriterMode::iAmABatchParticipant(txn.lockState());

        txn.getClient()->getAuthorizationSession()->grantInternalAuthorization();

        txn.getCurOp()->reset(HostAndPort(), dbInsert);
        NamespaceString ns(_index["ns"].String());

        Lock::DBLock dlk(txn.lockState(), ns.db(), MODE_X);
        Client::Context ctx(&txn, ns.getSystemIndexesCollection());

        Database* db = dbHolder().get(&txn, ns.db().toString());

        Status status = _build(&txn, db, true);
        if ( !status.isOK() ) {
            error() << "IndexBuilder could not build index: " << status.toString();
        }

        txn.getClient()->shutdown();
    }

    Status IndexBuilder::buildInForeground(OperationContext* txn, Database* db) const {
        return _build(txn, db, false);
    }

    Status IndexBuilder::_build(OperationContext* txn,
                                Database* db,
                                bool allowBackgroundBuilding) const {
        const string ns = _index["ns"].String();

        Collection* c = db->getCollection( txn, ns );
        if ( !c ) {
            WriteUnitOfWork wunit(txn);
            c = db->getOrCreateCollection( txn, ns );
            verify(c);
            wunit.commit();
        }

        // Show which index we're building in the curop display.
        txn->getCurOp()->setQuery(_index);

        MultiIndexBlock indexer(txn, c);
        indexer.allowInterruption();
        if (allowBackgroundBuilding)
            indexer.allowBackgroundBuilding();

        Status status = Status::OK();
        IndexDescriptor* descriptor(NULL);
        try {
            status = indexer.init(_index);
            if ( status.code() == ErrorCodes::IndexAlreadyExists )
                return Status::OK();

            if (allowBackgroundBuilding) {
                descriptor = indexer.registerIndexBuild();
            }

            if (status.isOK())
                status = indexer.insertAllDocumentsInCollection();

            if (status.isOK()) {
                WriteUnitOfWork wunit(txn);
                indexer.commit();
                wunit.commit();
            }
        }
        catch (const DBException& e) {
            status = e.toStatus();
        }
        
        if (status.code() == ErrorCodes::InterruptedAtShutdown) {
            // leave it as-if kill -9 happened. This will be handled on restart.
            indexer.abortWithoutCleanup();
        }

        if (allowBackgroundBuilding) {
            indexer.unregisterIndexBuild(descriptor);
        }
        return status;
    }

    std::vector<BSONObj> 
    IndexBuilder::killMatchingIndexBuilds(Collection* collection,
                                          const IndexCatalog::IndexKillCriteria& criteria) {
        invariant(collection);
        return collection->getIndexCatalog()->killMatchingIndexBuilds(criteria);
    }

    void IndexBuilder::restoreIndexes(const std::vector<BSONObj>& indexes) {
        log() << "restarting " << indexes.size() << " index build(s)" << endl;
        for (int i = 0; i < static_cast<int>(indexes.size()); i++) {
            IndexBuilder* indexBuilder = new IndexBuilder(indexes[i]);
            // This looks like a memory leak, but indexBuilder deletes itself when it finishes
            indexBuilder->go();
        }
    }
}

