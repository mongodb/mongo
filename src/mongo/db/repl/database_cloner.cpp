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

#include "mongo/db/repl/database_cloner.h"

#include <boost/thread/lock_guard.hpp>
#include <algorithm>
#include <iterator>
#include <set>

#include "mongo/db/catalog/collection_options.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {

namespace {

    const char* kNameFieldName = "name";
    const char* kOptionsFieldName = "options";

    /**
     * Default listCollections predicate.
     */
    bool acceptAllPred(const BSONObj&) {
        return true;
    }

    /**
     * Creates a listCollections command obj with an optional filter.
     */
    BSONObj createListCollectionsCommandObject(const BSONObj& filter) {
        BSONObjBuilder output;
        output.append("listCollections", 1);
        if (!filter.isEmpty()) {
            output.append("filter", filter);
        }
        return output.obj();
    }

} // namespace

    DatabaseCloner::DatabaseCloner(ReplicationExecutor* executor,
                                   const HostAndPort& source,
                                   const std::string& dbname,
                                   const BSONObj& listCollectionsFilter,
                                   const ListCollectionsPredicateFn& listCollectionsPred,
                                   const CreateStorageInterfaceFn& csi,
                                   const CollectionCallbackFn& collWork,
                                   const CallbackFn& work)
        : _executor(executor),
          _source(source),
          _dbname(dbname),
          _listCollectionsFilter(listCollectionsFilter),
          _listCollectionsPredicate(listCollectionsPred ? listCollectionsPred : acceptAllPred),
          _createStorageInterface(csi),
          _collectionWork(collWork),
          _work(work),
          _active(false),
          _listCollectionsFetcher(_executor,
                                  _source,
                                  _dbname,
                                  createListCollectionsCommandObject(_listCollectionsFilter),
                                  stdx::bind(&DatabaseCloner::_listCollectionsCallback,
                                             this,
                                             stdx::placeholders::_1,
                                             stdx::placeholders::_2)),
          // TODO: replace with executor database worker when it is available.
          _scheduleDbWorkFn(stdx::bind(&ReplicationExecutor::scheduleWorkWithGlobalExclusiveLock,
                                       _executor,
                                       stdx::placeholders::_1)),
          _startCollectionCloner([](CollectionCloner& cloner) { return cloner.start(); }) {

        uassert(ErrorCodes::BadValue, "null replication executor", executor);
        uassert(ErrorCodes::BadValue, "empty database name", !dbname.empty());
        uassert(ErrorCodes::BadValue, "storage interface creation function cannot be null", csi);
        uassert(ErrorCodes::BadValue, "collection callback function cannot be null", collWork);
        uassert(ErrorCodes::BadValue, "callback function cannot be null", work);
    }

    const std::vector<BSONObj>& DatabaseCloner::getCollectionInfos() const {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _collectionInfos;
    }

    std::string DatabaseCloner::getDiagnosticString() const {
        boost::lock_guard<boost::mutex> lk(_mutex);
        str::stream output;
        output << "DatabaseCloner";
        output << " executor: " << _executor->getDiagnosticString();
        output << " source: " << _source.toString();
        output << " database: " << _dbname;
        output << " listCollections filter" << _listCollectionsFilter;
        output << " active: " << _active;
        output << " collection info objects (empty if listCollections is in progress): "
               << _collectionInfos.size();
        return output;
    }

    bool DatabaseCloner::isActive() const {
        boost::lock_guard<boost::mutex> lk(_mutex);
        return _active;
    }

    Status DatabaseCloner::start() {
        boost::lock_guard<boost::mutex> lk(_mutex);

        if (_active) {
            return Status(ErrorCodes::IllegalOperation, "database cloner already started");
        }

        Status scheduleResult = _listCollectionsFetcher.schedule();
        if (!scheduleResult.isOK()) {
            return scheduleResult;
        }

        _active = true;

        return Status::OK();
    }

    void DatabaseCloner::cancel() {
        {
            boost::lock_guard<boost::mutex> lk(_mutex);

            if (!_active) {
                return;
            }
        }

        _listCollectionsFetcher.cancel();
    }

    void DatabaseCloner::wait() {
    }

    void DatabaseCloner::setScheduleDbWorkFn(const CollectionCloner::ScheduleDbWorkFn& work) {
        boost::lock_guard<boost::mutex> lk(_mutex);

        _scheduleDbWorkFn = work;
    }

    void DatabaseCloner::setStartCollectionClonerFn(
        const StartCollectionClonerFn& startCollectionCloner) {

        _startCollectionCloner = startCollectionCloner;
    }

    void DatabaseCloner::_listCollectionsCallback(const StatusWith<Fetcher::BatchData>& result,
                                                  Fetcher::NextAction* nextAction) {

        boost::lock_guard<boost::mutex> lk(_mutex);

        _active = false;

        if (!result.isOK()) {
            _work(result.getStatus());
            return;
        }

        auto&& documents = result.getValue().documents;

        // We may be called with multiple batches leading to a need to grow _collectionInfos.
        _collectionInfos.reserve(_collectionInfos.size() + documents.size());
        std::copy_if(documents.begin(), documents.end(),
                     std::back_inserter(_collectionInfos),
                     _listCollectionsPredicate);

        // The fetcher will continue to call with kContinue until an error or the last batch.
        if (*nextAction == Fetcher::NextAction::kContinue) {
            _active = true;
            return;
        }

        // Nothing to do for an empty database.
        if (_collectionInfos.empty()) {
            _work(Status::OK());
            return;
        }

        _collectionNamespaces.reserve(_collectionInfos.size());
        std::set<std::string> seen;
        for (auto&& info : _collectionInfos) {
            BSONElement nameElement = info.getField(kNameFieldName);
            if (nameElement.eoo()) {
                _work(Status(ErrorCodes::FailedToParse, str::stream() <<
                             "collection info must contain '" << kNameFieldName << "' " <<
                             "field : " << info));
                return;
            }
            if (nameElement.type() != mongo::String) {
                _work(Status(ErrorCodes::TypeMismatch, str::stream() <<
                             "'" << kNameFieldName << "' field must be a string: " << info));
                return;
            }
            const std::string collectionName = nameElement.String();
            if (seen.find(collectionName) != seen.end()) {
                _work(Status(ErrorCodes::DuplicateKey, str::stream() <<
                             "collection info contains duplicate collection name " <<
                             "'" << collectionName << "': " << info));
                return;
            }

            BSONElement optionsElement = info.getField(kOptionsFieldName);
            if (optionsElement.eoo()) {
                _work(Status(ErrorCodes::FailedToParse, str::stream() <<
                             "collection info must contain '" << kOptionsFieldName << "' " <<
                             "field : " << info));
                return;
            }
            if (!optionsElement.isABSONObj()) {
                _work(Status(ErrorCodes::TypeMismatch, str::stream() <<
                             "'" << kOptionsFieldName << "' field must be an object: " << info));
                return;
            }
            const BSONObj optionsObj = optionsElement.Obj();
            CollectionOptions options;
            Status parseStatus = options.parse(optionsObj);
            if (!parseStatus.isOK()) {
                _work(parseStatus);
                return;
            }
            seen.insert(collectionName);

            _collectionNamespaces.emplace_back(_dbname, collectionName);
            auto&& nss = *_collectionNamespaces.crbegin();

            try {
                _collectionCloners.emplace_back(
                    _executor,
                    _source,
                    nss,
                    options,
                    stdx::bind(&DatabaseCloner::_collectionClonerCallback,
                               this,
                               stdx::placeholders::_1,
                               nss),
                    _createStorageInterface());
            }
            catch (const UserException& ex) {
                _work(ex.toStatus());
                return;
            }
        }

        for (auto&& collectionCloner : _collectionCloners) {
            collectionCloner.setScheduleDbWorkFn(_scheduleDbWorkFn);
        }

        // Start first collection cloner.
        _currentCollectionClonerIter = _collectionCloners.begin();

        LOG(1) << "    cloning collection " << _currentCollectionClonerIter->getSourceNamespace();

        Status startStatus = _startCollectionCloner(*_currentCollectionClonerIter);
        if (!startStatus.isOK()) {
            LOG(1) << "    failed to start collection cloning on "
                   << _currentCollectionClonerIter->getSourceNamespace()
                   << ": " << startStatus;
            _work(startStatus);
            return;
        }

        _active = true;
    }

    void DatabaseCloner::_collectionClonerCallback(const Status& status,
                                                   const NamespaceString& nss) {
        boost::lock_guard<boost::mutex> lk(_mutex);

        _active = false;

        // Forward collection cloner result to caller.
        // Failure to clone a collection does not stop the database cloner
        // from cloning the rest of the collections in the listCollections result.
        _collectionWork(status, nss);

        _currentCollectionClonerIter++;

        LOG(1) << "    cloning collection " << _currentCollectionClonerIter->getSourceNamespace();

        if (_currentCollectionClonerIter != _collectionCloners.end()) {
            Status startStatus = _startCollectionCloner(*_currentCollectionClonerIter);
            if (!startStatus.isOK()) {
                LOG(1) << "    failed to start collection cloning on "
                       << _currentCollectionClonerIter->getSourceNamespace()
                       << ": " << startStatus;
                _work(startStatus);
                return;
            }
            _active = true;
            return;
        }

        _work(Status::OK());
    }

} // namespace repl
} // namespace mongo
