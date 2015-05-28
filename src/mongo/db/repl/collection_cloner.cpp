/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/collection_cloner.h"

#include <boost/thread/lock_guard.hpp>

#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

    CollectionCloner::CollectionCloner(ReplicationExecutor* executor,
                                       const HostAndPort& source,
                                       const NamespaceString& sourceNss,
                                       const CollectionOptions& options,
                                       const CallbackFn& work,
                                       StorageInterface* storageInterface)
        : _executor(executor),
          _source(source),
          _sourceNss(sourceNss),
          _destNss(_sourceNss),
          _options(options),
          _work(work),
          _storageInterface(storageInterface),
          _active(false),
          _listIndexesFetcher(_executor,
                              _source,
                              _sourceNss.db().toString(),
                              BSON("listIndexes" << _sourceNss.coll()),
                              stdx::bind(&CollectionCloner::_listIndexesCallback,
                                         this,
                                         stdx::placeholders::_1,
                                         stdx::placeholders::_2,
                                         stdx::placeholders::_3)),
         _findFetcher(_executor,
                      _source,
                      _sourceNss.db().toString(),
                      BSON("find" << _sourceNss.coll() <<
                           "noCursorTimeout" << true), // SERVER-1387
                      stdx::bind(&CollectionCloner::_findCallback,
                                 this,
                                 stdx::placeholders::_1,
                                 stdx::placeholders::_2,
                                 stdx::placeholders::_3)),
          _indexSpecs(),
          _documents(),
          _dbWorkCallbackHandle(),
          // TODO: replace with executor database worker when it is available.
          _scheduleDbWorkFn(stdx::bind(&ReplicationExecutor::scheduleWorkWithGlobalExclusiveLock,
                                       _executor,
                                       stdx::placeholders::_1)) {

        uassert(ErrorCodes::BadValue, "null replication executor", executor);
        uassert(ErrorCodes::BadValue, "invalid collection namespace: " + sourceNss.ns(),
                sourceNss.isValid());
        uassertStatusOK(options.validate());
        uassert(ErrorCodes::BadValue, "callback function cannot be null", work);
        uassert(ErrorCodes::BadValue, "null storage interface", storageInterface);
    }

    const NamespaceString& CollectionCloner::getSourceNamespace() const {
        return _sourceNss;
    }

    std::string CollectionCloner::getDiagnosticString() const {
        boost::lock_guard<boost::mutex> lk(_mutex);
        str::stream output;
        output << "CollectionCloner";
        output << " executor: " << _executor->getDiagnosticString();
        output << " source: " << _source.toString();
        output << " source namespace: " << _sourceNss.toString();
        output << " destination namespace: " << _destNss.toString();
        output << " collection options: " << _options.toBSON();
        output << " active: " << _active;
        output << " listIndexes fetcher: " << _listIndexesFetcher.getDiagnosticString();
        output << " find fetcher: " << _findFetcher.getDiagnosticString();
        output << " database worked callback handle: "
               << (_dbWorkCallbackHandle.isValid() ? "valid" : "invalid");
        return output;
    }

    bool CollectionCloner::isActive() const {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _active;
    }

    Status CollectionCloner::start() {
        boost::lock_guard<boost::mutex> lk(_mutex);

        if (_active) {
            return Status(ErrorCodes::IllegalOperation, "collection cloner already started");
        }

        Status scheduleResult = _listIndexesFetcher.schedule();
        if (!scheduleResult.isOK()) {
            return scheduleResult;
        }

        _active = true;

        return Status::OK();
    }

    void CollectionCloner::cancel() {
        ReplicationExecutor::CallbackHandle dbWorkCallbackHandle;
        {
            boost::lock_guard<boost::mutex> lk(_mutex);

            if (!_active) {
                return;
            }

            dbWorkCallbackHandle = _dbWorkCallbackHandle;
        }

        _listIndexesFetcher.cancel();
        _findFetcher.cancel();

        if (dbWorkCallbackHandle.isValid()) {
            _executor->cancel(dbWorkCallbackHandle);
        }
    }

    void CollectionCloner::wait() {
        // If a fetcher is inactive, wait() has no effect.
        _listIndexesFetcher.wait();
        _findFetcher.wait();
        waitForDbWorker();
    }

    void CollectionCloner::waitForDbWorker() {
        ReplicationExecutor::CallbackHandle dbWorkCallbackHandle;
        {
            boost::lock_guard<boost::mutex> lk(_mutex);

            if (!_active) {
                return;
            }

            dbWorkCallbackHandle = _dbWorkCallbackHandle;
        }

        if (dbWorkCallbackHandle.isValid()) {
            _executor->wait(dbWorkCallbackHandle);
        }
    }

    void CollectionCloner::setScheduleDbWorkFn(const ScheduleDbWorkFn& scheduleDbWorkFn) {
        boost::lock_guard<boost::mutex> lk(_mutex);

        _scheduleDbWorkFn = scheduleDbWorkFn;
    }

    void CollectionCloner::_listIndexesCallback(const StatusWith<Fetcher::BatchData>& fetchResult,
                                                Fetcher::NextAction* nextAction,
                                                BSONObjBuilder* getMoreBob) {
        boost::lock_guard<boost::mutex> lk(_mutex);

        _active = false;

        if (!fetchResult.isOK()) {
            _work(fetchResult.getStatus());
            return;
        }

        auto batchData(fetchResult.getValue());
        auto&& documents = batchData.documents;

        if (documents.empty()) {
            warning() << "No indexes found for collection " <<  _sourceNss.ns()
                      << " while cloning from " << _source;
        }

        // We may be called with multiple batches leading to a need to grow _indexSpecs.
        _indexSpecs.reserve(_indexSpecs.size() + documents.size());
        _indexSpecs.insert(_indexSpecs.end(), documents.begin(), documents.end());

        // The fetcher will continue to call with kGetMore until an error or the last batch.
        if (*nextAction == Fetcher::NextAction::kGetMore) {
            invariant(getMoreBob);
            getMoreBob->append("getMore", batchData.cursorId);
            getMoreBob->append("collection", batchData.nss.coll());

            _active = true;
            return;
        }

        // We have all of the indexes now, so we can start cloning the collection data.
        auto&& scheduleResult = _scheduleDbWorkFn(
            stdx::bind(&CollectionCloner::_beginCollectionCallback, this, stdx::placeholders::_1));
        if (!scheduleResult.isOK()) {
            _work(scheduleResult.getStatus());
            return;
        }

        _active = true;
        _dbWorkCallbackHandle = scheduleResult.getValue();
    }

    void CollectionCloner::_findCallback(const StatusWith<Fetcher::BatchData>& fetchResult,
                                         Fetcher::NextAction* nextAction,
                                         BSONObjBuilder* getMoreBob) {
        boost::lock_guard<boost::mutex> lk(_mutex);

        _active = false;

        if (!fetchResult.isOK()) {
            _work(fetchResult.getStatus());
            return;
        }

        auto batchData(fetchResult.getValue());
        _documents = batchData.documents;

        bool lastBatch = *nextAction == Fetcher::NextAction::kNoAction;
        auto&& scheduleResult = _scheduleDbWorkFn(stdx::bind(
            &CollectionCloner::_insertDocumentsCallback, this, stdx::placeholders::_1, lastBatch));
        if (!scheduleResult.isOK()) {
            _work(scheduleResult.getStatus());
            return;
        }

        if (*nextAction == Fetcher::NextAction::kGetMore) {
            invariant(getMoreBob);
            getMoreBob->append("getMore", batchData.cursorId);
            getMoreBob->append("collection", batchData.nss.coll());
        }

        _active = true;
        _dbWorkCallbackHandle = scheduleResult.getValue();
    }

    void CollectionCloner::_beginCollectionCallback(const ReplicationExecutor::CallbackData& cbd) {
        boost::lock_guard<boost::mutex> lk(_mutex);

        _active = false;

        if (!cbd.status.isOK()) {
            _work(cbd.status);
            return;
        }

        OperationContext* txn = cbd.txn;
        Status status = _storageInterface->beginCollection(txn, _destNss, _options, _indexSpecs);
        if (!status.isOK()) {
            _work(status);
            return;
        }

        Status scheduleStatus = _findFetcher.schedule();
        if (!scheduleStatus.isOK()) {
            _work(scheduleStatus);
            return;
        }

        _active = true;
    }

    void CollectionCloner::_insertDocumentsCallback(const ReplicationExecutor::CallbackData& cbd,
                                                    bool lastBatch) {
        boost::lock_guard<boost::mutex> lk(_mutex);

        _active = false;

        if (!cbd.status.isOK()) {
            _work(cbd.status);
            return;
        }

        Status status = _storageInterface->insertDocuments(cbd.txn, _destNss, _documents);
        if (!status.isOK()) {
            _work(status);
            return;
        }

        if (!lastBatch) {
            _active = true;
            return;
        }

        _work(Status::OK());
    }

} // namespace repl
} // namespace mongo
