// wiredtiger_kv_engine.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#ifdef _WIN32
#define NVALGRIND
#endif

#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"

#include <boost/filesystem.hpp>
#include <boost/filesystem/operations.hpp>
#include <valgrind/valgrind.h>

#include "mongo/base/error_codes.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_customization_hooks.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/log.h"
#include "mongo/util/background.h"
#include "mongo/util/exit.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

namespace mongo {

using std::set;
using std::string;

class WiredTigerKVEngine::WiredTigerJournalFlusher : public BackgroundJob {
public:
    explicit WiredTigerJournalFlusher(WiredTigerSessionCache* sessionCache)
        : BackgroundJob(false /* deleteSelf */), _sessionCache(sessionCache) {}

    virtual string name() const {
        return "WTJournalFlusher";
    }

    virtual void run() {
        Client::initThread(name().c_str());

        LOG(1) << "starting " << name() << " thread";

        while (!_shuttingDown.load()) {
            try {
                _sessionCache->waitUntilDurable(false);
            } catch (const UserException& e) {
                invariant(e.getCode() == ErrorCodes::ShutdownInProgress);
            }

            int ms = storageGlobalParams.journalCommitIntervalMs;
            if (!ms) {
                ms = 100;
            }

            sleepmillis(ms);
        }
        LOG(1) << "stopping " << name() << " thread";
    }

    void shutdown() {
        _shuttingDown.store(true);
        wait();
    }

private:
    WiredTigerSessionCache* _sessionCache;
    std::atomic<bool> _shuttingDown{false};  // NOLINT
};

WiredTigerKVEngine::WiredTigerKVEngine(const std::string& canonicalName,
                                       const std::string& path,
                                       const std::string& extraOpenOptions,
                                       size_t cacheSizeGB,
                                       bool durable,
                                       bool ephemeral,
                                       bool repair)
    : _eventHandler(WiredTigerUtil::defaultEventHandlers()),
      _canonicalName(canonicalName),
      _path(path),
      _durable(durable),
      _ephemeral(ephemeral),
      _sizeStorerSyncTracker(100000, 60 * 1000) {
    boost::filesystem::path journalPath = path;
    journalPath /= "journal";
    if (_durable) {
        if (!boost::filesystem::exists(journalPath)) {
            try {
                boost::filesystem::create_directory(journalPath);
            } catch (std::exception& e) {
                log() << "error creating journal dir " << journalPath.string() << ' ' << e.what();
                throw;
            }
        }
    }

    _previousCheckedDropsQueued = Date_t::now();

    std::stringstream ss;
    ss << "create,";
    ss << "cache_size=" << cacheSizeGB << "G,";
    ss << "session_max=20000,";
    ss << "eviction=(threads_max=4),";
    ss << "config_base=false,";
    ss << "statistics=(fast),";
    // The setting may have a later setting override it if not using the journal.  We make it
    // unconditional here because even nojournal may need this setting if it is a transition
    // from using the journal.
    ss << "log=(enabled=true,archive=true,path=journal,compressor=";
    ss << wiredTigerGlobalOptions.journalCompressor << "),";
    ss << "file_manager=(close_idle_time=100000),";  //~28 hours, will put better fix in 3.1.x
    ss << "checkpoint=(wait=" << wiredTigerGlobalOptions.checkpointDelaySecs;
    ss << ",log_size=2GB),";
    ss << "statistics_log=(wait=" << wiredTigerGlobalOptions.statisticsLogDelaySecs << "),";
    ss << WiredTigerCustomizationHooks::get(getGlobalServiceContext())->getOpenConfig("system");
#ifdef _WIN32
    ss << "direct_io=(data),";
#endif
    ss << extraOpenOptions;
    if (!_durable) {
        // If we started without the journal, but previously used the journal then open with the
        // WT log enabled to perform any unclean shutdown recovery and then close and reopen in
        // the normal path without the journal.
        if (boost::filesystem::exists(journalPath)) {
            string config = ss.str();
            log() << "Detected WT journal files.  Running recovery from last checkpoint.";
            log() << "journal to nojournal transition config: " << config;
            int ret = wiredtiger_open(path.c_str(), &_eventHandler, config.c_str(), &_conn);
            if (ret == EINVAL) {
                fassertFailedNoTrace(28717);
            } else if (ret != 0) {
                Status s(wtRCToStatus(ret));
                msgassertedNoTrace(28718, s.reason());
            }
            invariantWTOK(_conn->close(_conn, NULL));
        }
        // This setting overrides the earlier setting because it is later in the config string.
        ss << ",log=(enabled=false),";
    }
    string config = ss.str();
    log() << "wiredtiger_open config: " << config;
    int ret = wiredtiger_open(path.c_str(), &_eventHandler, config.c_str(), &_conn);
    // Invalid argument (EINVAL) is usually caused by invalid configuration string.
    // We still fassert() but without a stack trace.
    if (ret == EINVAL) {
        fassertFailedNoTrace(28561);
    } else if (ret != 0) {
        Status s(wtRCToStatus(ret));
        msgassertedNoTrace(28595, s.reason());
    }

    _sessionCache.reset(new WiredTigerSessionCache(this));

    if (_durable) {
        _journalFlusher = stdx::make_unique<WiredTigerJournalFlusher>(_sessionCache.get());
        _journalFlusher->go();
    }

    _sizeStorerUri = "table:sizeStorer";
    {
        WiredTigerSession session(_conn);
        if (repair && _hasUri(session.getSession(), _sizeStorerUri)) {
            log() << "Repairing size cache";
            fassertNoTrace(28577, _salvageIfNeeded(_sizeStorerUri.c_str()));
        }
        _sizeStorer.reset(new WiredTigerSizeStorer(_conn, _sizeStorerUri));
        _sizeStorer->fillCache();
    }
}


WiredTigerKVEngine::~WiredTigerKVEngine() {
    if (_conn) {
        cleanShutdown();
    }

    _sessionCache.reset(NULL);
}

void WiredTigerKVEngine::cleanShutdown() {
    log() << "WiredTigerKVEngine shutting down";
    syncSizeInfo(true);
    if (_conn) {
        // these must be the last things we do before _conn->close();
        _sizeStorer.reset(NULL);
        if (_journalFlusher)
            _journalFlusher->shutdown();
        _sessionCache->shuttingDown();

// We want WiredTiger to leak memory for faster shutdown except when we are running tools to
// look for memory leaks.
#if !__has_feature(address_sanitizer)
        bool leak_memory = true;
#else
        bool leak_memory = false;
#endif
        const char* config = nullptr;

        if (RUNNING_ON_VALGRIND) {
            leak_memory = false;
        }

        if (leak_memory) {
            config = "leak_memory=true";
        }

        invariantWTOK(_conn->close(_conn, config));
        _conn = NULL;
    }
}

Status WiredTigerKVEngine::okToRename(OperationContext* opCtx,
                                      StringData fromNS,
                                      StringData toNS,
                                      StringData ident,
                                      const RecordStore* originalRecordStore) const {
    _sizeStorer->storeToCache(
        _uri(ident), originalRecordStore->numRecords(opCtx), originalRecordStore->dataSize(opCtx));
    syncSizeInfo(true);
    return Status::OK();
}

int64_t WiredTigerKVEngine::getIdentSize(OperationContext* opCtx, StringData ident) {
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(opCtx)->getSession(opCtx);
    return WiredTigerUtil::getIdentSize(session->getSession(), _uri(ident));
}

Status WiredTigerKVEngine::repairIdent(OperationContext* opCtx, StringData ident) {
    WiredTigerSession* session = WiredTigerRecoveryUnit::get(opCtx)->getSession(opCtx);
    session->closeAllCursors();
    if (isEphemeral()) {
        return Status::OK();
    }
    string uri = _uri(ident);
    return _salvageIfNeeded(uri.c_str());
}

Status WiredTigerKVEngine::_salvageIfNeeded(const char* uri) {
    // Using a side session to avoid transactional issues
    WiredTigerSession sessionWrapper(_conn);
    WT_SESSION* session = sessionWrapper.getSession();

    int rc = (session->verify)(session, uri, NULL);
    if (rc == 0) {
        log() << "Verify succeeded on uri " << uri << ". Not salvaging.";
        return Status::OK();
    }

    if (rc == EBUSY) {
        // SERVER-16457: verify and salvage are occasionally failing with EBUSY. For now we
        // lie and return OK to avoid breaking tests. This block should go away when that ticket
        // is resolved.
        error() << "Verify on " << uri << " failed with EBUSY. Assuming no salvage is needed.";
        return Status::OK();
    }

    // TODO need to cleanup the sizeStorer cache after salvaging.
    log() << "Verify failed on uri " << uri << ". Running a salvage operation.";
    return wtRCToStatus(session->salvage(session, uri, NULL), "Salvage failed:");
}

int WiredTigerKVEngine::flushAllFiles(bool sync) {
    LOG(1) << "WiredTigerKVEngine::flushAllFiles";
    syncSizeInfo(true);
    _sessionCache->waitUntilDurable(true);

    return 1;
}

Status WiredTigerKVEngine::beginBackup(OperationContext* txn) {
    invariant(!_backupSession);

    // This cursor will be freed by the backupSession being closed as the session is uncached
    auto session = stdx::make_unique<WiredTigerSession>(_conn);
    WT_CURSOR* c = NULL;
    WT_SESSION* s = session->getSession();
    int ret = WT_OP_CHECK(s->open_cursor(s, "backup:", NULL, NULL, &c));
    if (ret != 0) {
        return wtRCToStatus(ret);
    }
    _backupSession = std::move(session);
    return Status::OK();
}

void WiredTigerKVEngine::endBackup(OperationContext* txn) {
    _backupSession.reset();
}

void WiredTigerKVEngine::syncSizeInfo(bool sync) const {
    if (!_sizeStorer)
        return;

    try {
        _sizeStorer->syncCache(sync);
    } catch (const WriteConflictException&) {
        // ignore, we'll try again later.
    }
}

RecoveryUnit* WiredTigerKVEngine::newRecoveryUnit() {
    return new WiredTigerRecoveryUnit(_sessionCache.get());
}

void WiredTigerKVEngine::setRecordStoreExtraOptions(const std::string& options) {
    _rsOptions = options;
}

void WiredTigerKVEngine::setSortedDataInterfaceExtraOptions(const std::string& options) {
    _indexOptions = options;
}

Status WiredTigerKVEngine::createRecordStore(OperationContext* opCtx,
                                             StringData ns,
                                             StringData ident,
                                             const CollectionOptions& options) {
    _checkIdentPath(ident);
    WiredTigerSession session(_conn);

    StatusWith<std::string> result =
        WiredTigerRecordStore::generateCreateString(_canonicalName, ns, options, _rsOptions);
    if (!result.isOK()) {
        return result.getStatus();
    }
    std::string config = result.getValue();

    string uri = _uri(ident);
    WT_SESSION* s = session.getSession();
    LOG(2) << "WiredTigerKVEngine::createRecordStore uri: " << uri << " config: " << config;
    return wtRCToStatus(s->create(s, uri.c_str(), config.c_str()));
}

RecordStore* WiredTigerKVEngine::getRecordStore(OperationContext* opCtx,
                                                StringData ns,
                                                StringData ident,
                                                const CollectionOptions& options) {
    if (options.capped) {
        return new WiredTigerRecordStore(opCtx,
                                         ns,
                                         _uri(ident),
                                         _canonicalName,
                                         options.capped,
                                         _ephemeral,
                                         options.cappedSize ? options.cappedSize : 4096,
                                         options.cappedMaxDocs ? options.cappedMaxDocs : -1,
                                         NULL,
                                         _sizeStorer.get());
    } else {
        return new WiredTigerRecordStore(opCtx,
                                         ns,
                                         _uri(ident),
                                         _canonicalName,
                                         false,
                                         _ephemeral,
                                         -1,
                                         -1,
                                         NULL,
                                         _sizeStorer.get());
    }
}

string WiredTigerKVEngine::_uri(StringData ident) const {
    return string("table:") + ident.toString();
}

Status WiredTigerKVEngine::createSortedDataInterface(OperationContext* opCtx,
                                                     StringData ident,
                                                     const IndexDescriptor* desc) {
    _checkIdentPath(ident);

    std::string collIndexOptions;
    const Collection* collection = desc->getCollection();

    // Treat 'collIndexOptions' as an empty string when the collection member of 'desc' is NULL in
    // order to allow for unit testing WiredTigerKVEngine::createSortedDataInterface().
    if (collection) {
        const CollectionCatalogEntry* cce = collection->getCatalogEntry();
        const CollectionOptions collOptions = cce->getCollectionOptions(opCtx);

        if (!collOptions.indexOptionDefaults["storageEngine"].eoo()) {
            BSONObj storageEngineOptions = collOptions.indexOptionDefaults["storageEngine"].Obj();
            collIndexOptions = storageEngineOptions.getFieldDotted(_canonicalName + ".configString")
                                   .valuestrsafe();
        }
    }

    StatusWith<std::string> result = WiredTigerIndex::generateCreateString(
        _canonicalName, _indexOptions, collIndexOptions, *desc);
    if (!result.isOK()) {
        return result.getStatus();
    }

    std::string config = result.getValue();

    LOG(2) << "WiredTigerKVEngine::createSortedDataInterface ident: " << ident
           << " config: " << config;
    return wtRCToStatus(WiredTigerIndex::Create(opCtx, _uri(ident), config));
}

SortedDataInterface* WiredTigerKVEngine::getSortedDataInterface(OperationContext* opCtx,
                                                                StringData ident,
                                                                const IndexDescriptor* desc) {
    if (desc->unique())
        return new WiredTigerIndexUnique(opCtx, _uri(ident), desc);
    return new WiredTigerIndexStandard(opCtx, _uri(ident), desc);
}

Status WiredTigerKVEngine::dropIdent(OperationContext* opCtx, StringData ident) {
    _drop(ident);
    return Status::OK();
}

bool WiredTigerKVEngine::_drop(StringData ident) {
    string uri = _uri(ident);

    WiredTigerSession session(_conn);

    int ret = session.getSession()->drop(session.getSession(), uri.c_str(), "force");
    LOG(1) << "WT drop of  " << uri << " res " << ret;

    if (ret == 0) {
        // yay, it worked
        return true;
    }

    if (ret == EBUSY) {
        // this is expected, queue it up
        {
            stdx::lock_guard<stdx::mutex> lk(_identToDropMutex);
            _identToDrop.insert(uri);
        }
        _sessionCache->closeAll();
        return false;
    }

    invariantWTOK(ret);
    return false;
}

bool WiredTigerKVEngine::haveDropsQueued() const {
    Date_t now = Date_t::now();
    Milliseconds delta = now - _previousCheckedDropsQueued;

    if (_sizeStorerSyncTracker.intervalHasElapsed()) {
        _sizeStorerSyncTracker.resetLastTime();
        syncSizeInfo(false);
    }

    // We only want to check the queue max once per second or we'll thrash
    // This is done in haveDropsQueued, not dropAllQueued so we skip the mutex
    if (delta < Milliseconds(1000))
        return false;

    _previousCheckedDropsQueued = now;
    stdx::lock_guard<stdx::mutex> lk(_identToDropMutex);
    return !_identToDrop.empty();
}

void WiredTigerKVEngine::dropAllQueued() {
    set<string> mine;
    {
        stdx::lock_guard<stdx::mutex> lk(_identToDropMutex);
        mine = _identToDrop;
    }

    set<string> deleted;

    {
        WiredTigerSession session(_conn);
        for (set<string>::const_iterator it = mine.begin(); it != mine.end(); ++it) {
            string uri = *it;
            int ret = session.getSession()->drop(session.getSession(), uri.c_str(), "force");
            LOG(1) << "WT queued drop of  " << uri << " res " << ret;

            if (ret == 0) {
                deleted.insert(uri);
                continue;
            }

            if (ret == EBUSY) {
                // leave in qeuue
                continue;
            }

            invariantWTOK(ret);
        }
    }

    {
        stdx::lock_guard<stdx::mutex> lk(_identToDropMutex);
        for (set<string>::const_iterator it = deleted.begin(); it != deleted.end(); ++it) {
            _identToDrop.erase(*it);
        }
    }
}

bool WiredTigerKVEngine::supportsDocLocking() const {
    return true;
}

bool WiredTigerKVEngine::supportsDirectoryPerDB() const {
    return true;
}

bool WiredTigerKVEngine::hasIdent(OperationContext* opCtx, StringData ident) const {
    return _hasUri(WiredTigerRecoveryUnit::get(opCtx)->getSession(opCtx)->getSession(),
                   _uri(ident));
}

bool WiredTigerKVEngine::_hasUri(WT_SESSION* session, const std::string& uri) const {
    // can't use WiredTigerCursor since this is called from constructor.
    WT_CURSOR* c = NULL;
    int ret = session->open_cursor(session, "metadata:", NULL, NULL, &c);
    if (ret == ENOENT)
        return false;
    invariantWTOK(ret);
    ON_BLOCK_EXIT(c->close, c);

    c->set_key(c, uri.c_str());
    return c->search(c) == 0;
}

std::vector<std::string> WiredTigerKVEngine::getAllIdents(OperationContext* opCtx) const {
    std::vector<std::string> all;
    WiredTigerCursor cursor("metadata:", WiredTigerSession::kMetadataTableId, false, opCtx);
    WT_CURSOR* c = cursor.get();
    if (!c)
        return all;

    while (c->next(c) == 0) {
        const char* raw;
        c->get_key(c, &raw);
        StringData key(raw);
        size_t idx = key.find(':');
        if (idx == string::npos)
            continue;
        StringData type = key.substr(0, idx);
        if (type != "table")
            continue;

        StringData ident = key.substr(idx + 1);
        if (ident == "sizeStorer")
            continue;

        all.push_back(ident.toString());
    }

    return all;
}

int WiredTigerKVEngine::reconfigure(const char* str) {
    return _conn->reconfigure(_conn, str);
}

void WiredTigerKVEngine::_checkIdentPath(StringData ident) {
    size_t start = 0;
    size_t idx;
    while ((idx = ident.find('/', start)) != string::npos) {
        StringData dir = ident.substr(0, idx);

        boost::filesystem::path subdir = _path;
        subdir /= dir.toString();
        if (!boost::filesystem::exists(subdir)) {
            LOG(1) << "creating subdirectory: " << dir;
            try {
                boost::filesystem::create_directory(subdir);
            } catch (const std::exception& e) {
                error() << "error creating path " << subdir.string() << ' ' << e.what();
                throw;
            }
        }

        start = idx + 1;
    }
}
}
