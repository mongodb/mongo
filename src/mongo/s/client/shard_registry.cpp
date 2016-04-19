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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/s/client/shard_registry.h"

#include <set>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/query_fetcher.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/client.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/catalog/catalog_manager.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/grid.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/write_ops/wc_error_detail.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/log.h"
#include "mongo/util/map_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

namespace mongo {

using std::shared_ptr;
using std::set;
using std::string;
using std::unique_ptr;
using std::vector;

using executor::TaskExecutor;
using RemoteCommandCallbackArgs = TaskExecutor::RemoteCommandCallbackArgs;
using repl::OpTime;

namespace {

const char kCmdResponseWriteConcernField[] = "writeConcernError";

const Seconds kConfigCommandTimeout{30};
const int kOnErrorNumRetries = 3;

// TODO: This has been moved into Shard(Remote). Remove this from here once
// ShardRegistry::runIdempotentCommandOnConfig and ShardRegistry::runCommandOnConfigWithRetries
// are removed.
const BSONObj kReplMetadata(BSON(rpc::kReplSetMetadataFieldName << 1));

// TODO: This has been moved into Shard(Remote). Remove this from here once
// ShardRegistry::runIdempotentCommandOnShard is removed.
const BSONObj kSecondaryOkMetadata{rpc::ServerSelectionMetadata(true, boost::none).toBSON()};

// TODO: This has been moved into Shard(Remote). Remove this from here once
// ShardRegistry::runIdempotentCommandOnConfig is removed.
const BSONObj kReplSecondaryOkMetadata{[] {
    BSONObjBuilder o;
    o.appendElements(kSecondaryOkMetadata);
    o.appendElements(kReplMetadata);
    return o.obj();
}()};

BSONObj appendMaxTimeToCmdObj(long long maxTimeMicros, const BSONObj& cmdObj) {
    Seconds maxTime = kConfigCommandTimeout;

    Microseconds remainingTxnMaxTime(maxTimeMicros);
    bool hasTxnMaxTime(remainingTxnMaxTime != Microseconds::zero());
    bool hasUserMaxTime = !cmdObj[LiteParsedQuery::cmdOptionMaxTimeMS].eoo();

    if (hasTxnMaxTime) {
        maxTime = duration_cast<Seconds>(remainingTxnMaxTime);
    } else if (hasUserMaxTime) {
        return cmdObj;
    }

    BSONObjBuilder updatedCmdBuilder;
    if (hasTxnMaxTime && hasUserMaxTime) {  // Need to remove user provided maxTimeMS.
        BSONObjIterator cmdObjIter(cmdObj);
        const char* maxTimeFieldName = LiteParsedQuery::cmdOptionMaxTimeMS;
        while (cmdObjIter.more()) {
            BSONElement e = cmdObjIter.next();
            if (str::equals(e.fieldName(), maxTimeFieldName)) {
                continue;
            }
            updatedCmdBuilder.append(e);
        }
    } else {
        updatedCmdBuilder.appendElements(cmdObj);
    }

    updatedCmdBuilder.append(LiteParsedQuery::cmdOptionMaxTimeMS,
                             durationCount<Milliseconds>(maxTime));
    return updatedCmdBuilder.obj();
}

Status checkForWriteConcernError(const BSONObj& obj) {
    BSONElement wcErrorElem;
    Status status = bsonExtractTypedField(obj, kCmdResponseWriteConcernField, Object, &wcErrorElem);
    if (status.isOK()) {
        BSONObj wcErrObj(wcErrorElem.Obj());

        WCErrorDetail wcError;
        string wcErrorParseMsg;
        if (!wcError.parseBSON(wcErrObj, &wcErrorParseMsg)) {
            return Status(ErrorCodes::UnsupportedFormat,
                          str::stream() << "Failed to parse write concern section due to "
                                        << wcErrorParseMsg);
        } else {
            return Status(ErrorCodes::WriteConcernFailed, wcError.toString());
        }
    } else if (status == ErrorCodes::NoSuchKey) {
        return Status::OK();
    }

    return status;
}

}  // unnamed namespace

const ShardRegistry::ErrorCodesSet ShardRegistry::kNotMasterErrors{ErrorCodes::NotMaster,
                                                                   ErrorCodes::NotMasterNoSlaveOk};
const ShardRegistry::ErrorCodesSet ShardRegistry::kAllRetriableErrors{
    ErrorCodes::NotMaster,
    ErrorCodes::NotMasterNoSlaveOk,
    ErrorCodes::NotMasterOrSecondary,
    // If write concern failed to be satisfied on the remote server, this most probably means that
    // some of the secondary nodes were unreachable or otherwise unresponsive, so the call is safe
    // to be retried if idempotency can be guaranteed.
    ErrorCodes::WriteConcernFailed,
    ErrorCodes::HostUnreachable,
    ErrorCodes::HostNotFound,
    ErrorCodes::NetworkTimeout,
    ErrorCodes::InterruptedDueToReplStateChange};

const ShardRegistry::ErrorCodesSet ShardRegistry::kWriteConcernErrors{
    ErrorCodes::WriteConcernFailed,
    ErrorCodes::WriteConcernLegacyOK,
    ErrorCodes::UnknownReplWriteConcern,
    ErrorCodes::CannotSatisfyWriteConcern};

ShardRegistry::ShardRegistry(std::unique_ptr<ShardFactory> shardFactory,
                             ConnectionString configServerCS)
    : _shardFactory(std::move(shardFactory)) {
    updateConfigServerConnectionString(configServerCS);
}

ShardRegistry::~ShardRegistry() = default;

ConnectionString ShardRegistry::getConfigServerConnectionString() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _configServerCS;
}

void ShardRegistry::updateConfigServerConnectionString(ConnectionString configServerCS) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _updateConfigServerConnectionString_inlock(std::move(configServerCS));
}

void ShardRegistry::_updateConfigServerConnectionString_inlock(ConnectionString configServerCS) {
    log() << "Updating config server connection string to: " << configServerCS.toString();

    _configServerCS = std::move(configServerCS);
    _addConfigShard_inlock();
}

bool ShardRegistry::reload(OperationContext* txn) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    if (_reloadState == ReloadState::Reloading) {
        // Another thread is already in the process of reloading so no need to do duplicate work.
        // There is also an issue if multiple threads are allowed to call getAllShards()
        // simultaneously because there is no good way to determine which of the threads has the
        // more recent version of the data.
        do {
            _inReloadCV.wait(lk);
        } while (_reloadState == ReloadState::Reloading);

        if (_reloadState == ReloadState::Idle) {
            return false;
        }
        // else proceed to reload since an error occured on the last reload attempt.
        invariant(_reloadState == ReloadState::Failed);
    }

    _reloadState = ReloadState::Reloading;
    lk.unlock();

    auto nextReloadState = ReloadState::Failed;
    auto failGuard = MakeGuard([&] {
        if (!lk.owns_lock()) {
            lk.lock();
        }
        _reloadState = nextReloadState;
        _inReloadCV.notify_all();
    });

    auto shardsStatus = grid.catalogManager(txn)->getAllShards(txn);

    if (!shardsStatus.isOK()) {
        uasserted(shardsStatus.getStatus().code(),
                  str::stream() << "could not get updated shard list from config server due to "
                                << shardsStatus.getStatus().reason());
    }

    auto shards = std::move(shardsStatus.getValue().value);
    auto reloadOpTime = std::move(shardsStatus.getValue().opTime);

    LOG(1) << "found " << shards.size()
           << " shards listed on config server(s) with lastVisibleOpTime: "
           << reloadOpTime.toBSON();

    // Ensure targeter exists for all shards and take shard connection string from the targeter.
    // Do this before re-taking the mutex to avoid deadlock with the ReplicaSetMonitor updating
    // hosts for a given shard.
    std::vector<std::tuple<std::string, ConnectionString>> shardsInfo;
    for (const auto& shardType : shards) {
        // This validation should ideally go inside the ShardType::validate call. However, doing
        // it there would prevent us from loading previously faulty shard hosts, which might have
        // been stored (i.e., the entire getAllShards call would fail).
        auto shardHostStatus = ConnectionString::parse(shardType.getHost());
        if (!shardHostStatus.isOK()) {
            warning() << "Unable to parse shard host " << shardHostStatus.getStatus().toString();
            continue;
        }

        shardsInfo.push_back(std::make_tuple(shardType.getName(), shardHostStatus.getValue()));
    }

    lk.lock();

    _lookup.clear();
    _rsLookup.clear();
    _hostLookup.clear();

    _addConfigShard_inlock();

    for (auto& shardInfo : shardsInfo) {
        // Skip the config host even if there is one left over from legacy installations. The
        // config host is installed manually from the catalog manager data.
        if (std::get<0>(shardInfo) == "config") {
            continue;
        }

        _addShard_inlock(std::move(std::get<0>(shardInfo)), std::move(std::get<1>(shardInfo)));
    }

    nextReloadState = ReloadState::Idle;
    return true;
}

void ShardRegistry::rebuildConfigShard() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _addConfigShard_inlock();
}

shared_ptr<Shard> ShardRegistry::getShard(OperationContext* txn, const ShardId& shardId) {
    shared_ptr<Shard> shard = _findUsingLookUp(shardId);
    if (shard) {
        return shard;
    }

    // If we can't find the shard, we might just need to reload the cache
    bool didReload = reload(txn);

    shard = _findUsingLookUp(shardId);

    if (shard || didReload) {
        return shard;
    }

    reload(txn);
    return _findUsingLookUp(shardId);
}

shared_ptr<Shard> ShardRegistry::getShardNoReload(const ShardId& shardId) {
    return _findUsingLookUp(shardId);
}

shared_ptr<Shard> ShardRegistry::getShardForHostNoReload(const HostAndPort& host) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return mapFindWithDefault(_hostLookup, host);
}

shared_ptr<Shard> ShardRegistry::getConfigShard() {
    shared_ptr<Shard> shard = _findUsingLookUp("config");
    invariant(shard);
    return shard;
}

unique_ptr<Shard> ShardRegistry::createConnection(const ConnectionString& connStr) const {
    return _shardFactory->createUniqueShard("<unnamed>", connStr, false);
}

shared_ptr<Shard> ShardRegistry::lookupRSName(const string& name) const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    ShardMap::const_iterator i = _rsLookup.find(name);

    return (i == _rsLookup.end()) ? nullptr : i->second;
}

void ShardRegistry::remove(const ShardId& id) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    set<string> entriesToRemove;
    for (const auto& i : _lookup) {
        shared_ptr<Shard> s = i.second;
        if (s->getId() == id) {
            entriesToRemove.insert(i.first);
            ConnectionString connStr = s->getConnString();
            for (const auto& host : connStr.getServers()) {
                entriesToRemove.insert(host.toString());
                _hostLookup.erase(host);
            }
        }
    }
    for (const auto& entry : entriesToRemove) {
        _lookup.erase(entry);
    }

    for (ShardMap::iterator i = _rsLookup.begin(); i != _rsLookup.end();) {
        shared_ptr<Shard> s = i->second;
        if (s->getId() == id) {
            _rsLookup.erase(i++);
        } else {
            ++i;
        }
    }

    shardConnectionPool.removeHost(id);
    ReplicaSetMonitor::remove(id);
}

void ShardRegistry::getAllShardIds(vector<ShardId>* all) const {
    std::set<string> seen;

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        for (ShardMap::const_iterator i = _lookup.begin(); i != _lookup.end(); ++i) {
            const shared_ptr<Shard>& s = i->second;
            if (s->getId() == "config") {
                continue;
            }

            seen.insert(s->getId());
        }
    }

    all->assign(seen.begin(), seen.end());
}

void ShardRegistry::toBSON(BSONObjBuilder* result) {
    // Need to copy, then sort by shardId.
    std::vector<std::pair<ShardId, std::string>> shards;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        shards.reserve(_lookup.size());
        for (auto&& shard : _lookup) {
            shards.emplace_back(shard.first, shard.second->getConnString().toString());
        }
    }

    std::sort(std::begin(shards), std::end(shards));

    BSONObjBuilder mapBob(result->subobjStart("map"));
    for (auto&& shard : shards) {
        mapBob.append(shard.first, shard.second);
    }
}

void ShardRegistry::_addConfigShard_inlock() {
    _addShard_inlock("config", _configServerCS);
}

void ShardRegistry::updateReplSetHosts(const ConnectionString& newConnString) {
    invariant(newConnString.type() == ConnectionString::SET ||
              newConnString.type() == ConnectionString::CUSTOM);  // For dbtests

    stdx::lock_guard<stdx::mutex> lk(_mutex);
    ShardMap::const_iterator i = _rsLookup.find(newConnString.getSetName());
    if (i == _rsLookup.end())
        return;
    auto shard = i->second;
    if (shard->isConfig()) {
        _updateConfigServerConnectionString_inlock(newConnString);
    } else {
        _addShard_inlock(shard->getId(), newConnString);
    }
}

void ShardRegistry::_addShard_inlock(const ShardId& shardId, const ConnectionString& connString) {
    auto currentShard = _findUsingLookUp_inlock(shardId);
    if (currentShard) {
        auto oldConnString = currentShard->originalConnString();

        if (oldConnString.toString() != connString.toString()) {
            log() << "Updating ShardRegistry connection string for shard " << currentShard->getId()
                  << " from: " << oldConnString.toString() << " to: " << connString.toString();
        }

        for (const auto& host : oldConnString.getServers()) {
            _lookup.erase(host.toString());
            _hostLookup.erase(host);
        }
    }

    // TODO: the third argument should pass the bool that will instruct factory to create either
    // local or remote shard.
    auto shard = _shardFactory->createShard(shardId, connString, false);

    _lookup[shard->getId()] = shard;

    if (connString.type() == ConnectionString::SET) {
        _rsLookup[connString.getSetName()] = shard;
    } else if (connString.type() == ConnectionString::CUSTOM) {
        // CUSTOM connection strings (ie "$dummy:10000) become DBDirectClient connections which
        // always return "localhost" as their resposne to getServerAddress().  This is just for
        // making dbtest work.
        _lookup["localhost"] = shard;
        _hostLookup[HostAndPort{"localhost"}] = shard;
    }

    // TODO: The only reason to have the shard host names in the lookup table is for the
    // setShardVersion call, which resolves the shard id from the shard address. This is
    // error-prone and will go away eventually when we switch all communications to go through
    // the remote command runner and all nodes are sharding aware by default.
    _lookup[connString.toString()] = shard;

    for (const HostAndPort& hostAndPort : connString.getServers()) {
        _lookup[hostAndPort.toString()] = shard;
        _hostLookup[hostAndPort] = shard;
    }
}

shared_ptr<Shard> ShardRegistry::_findUsingLookUp(const ShardId& shardId) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _findUsingLookUp_inlock(shardId);
}

shared_ptr<Shard> ShardRegistry::_findUsingLookUp_inlock(const ShardId& shardId) {
    ShardMap::iterator it = _lookup.find(shardId);
    if (it != _lookup.end()) {
        return it->second;
    }

    return nullptr;
}

StatusWith<Shard::QueryResponse> ShardRegistry::exhaustiveFindOnConfig(
    OperationContext* txn,
    const ReadPreferenceSetting& readPref,
    const NamespaceString& nss,
    const BSONObj& query,
    const BSONObj& sort,
    boost::optional<long long> limit) {
    for (int retry = 1; retry <= kOnErrorNumRetries; retry++) {
        const auto configShard = getConfigShard();
        auto result = configShard->exhaustiveFindOnConfig(txn, readPref, nss, query, sort, limit);
        if (result.isOK()) {
            return result;
        }

        if (kAllRetriableErrors.count(result.getStatus().code()) && retry < kOnErrorNumRetries) {
            continue;
        }

        return result.getStatus();
    }

    MONGO_UNREACHABLE;
}

StatusWith<BSONObj> ShardRegistry::runIdempotentCommandOnShard(
    OperationContext* txn,
    const std::shared_ptr<Shard>& shard,
    const ReadPreferenceSetting& readPref,
    const std::string& dbName,
    const BSONObj& cmdObj) {
    auto response = _runCommandWithRetries(txn,
                                           Grid::get(txn)->getExecutorPool()->getFixedExecutor(),
                                           shard,
                                           readPref,
                                           dbName,
                                           cmdObj,
                                           readPref.pref == ReadPreference::PrimaryOnly
                                               ? rpc::makeEmptyMetadata()
                                               : kSecondaryOkMetadata,
                                           kAllRetriableErrors);
    if (!response.isOK()) {
        return response.getStatus();
    }

    return response.getValue().response;
}

StatusWith<BSONObj> ShardRegistry::runIdempotentCommandOnShard(
    OperationContext* txn,
    ShardId shardId,
    const ReadPreferenceSetting& readPref,
    const std::string& dbName,
    const BSONObj& cmdObj) {
    auto shard = getShard(txn, shardId);
    if (!shard) {
        return {ErrorCodes::ShardNotFound, str::stream() << "shard " << shardId << " not found"};
    }
    return runIdempotentCommandOnShard(txn, shard, readPref, dbName, cmdObj);
}

StatusWith<BSONObj> ShardRegistry::runIdempotentCommandOnConfig(
    OperationContext* txn,
    const ReadPreferenceSetting& readPref,
    const std::string& dbName,
    const BSONObj& cmdObj) {
    auto response = _runCommandWithRetries(
        txn,
        Grid::get(txn)->getExecutorPool()->getFixedExecutor(),
        getConfigShard(),
        readPref,
        dbName,
        cmdObj,
        readPref.pref == ReadPreference::PrimaryOnly ? kReplMetadata : kReplSecondaryOkMetadata,
        kAllRetriableErrors);

    if (!response.isOK()) {
        return response.getStatus();
    }

    return response.getValue().response;
}

StatusWith<BSONObj> ShardRegistry::runCommandOnConfigWithRetries(
    OperationContext* txn,
    const std::string& dbname,
    const BSONObj& cmdObj,
    const ShardRegistry::ErrorCodesSet& errorsToCheck) {
    auto response = _runCommandWithRetries(txn,
                                           Grid::get(txn)->getExecutorPool()->getFixedExecutor(),
                                           getConfigShard(),
                                           ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                           dbname,
                                           cmdObj,
                                           kReplMetadata,
                                           errorsToCheck);
    if (!response.isOK()) {
        return response.getStatus();
    }

    return response.getValue().response;
}

StatusWith<Shard::CommandResponse> ShardRegistry::_runCommandWithRetries(
    OperationContext* txn,
    TaskExecutor* executor,
    const std::shared_ptr<Shard>& shard,
    const ReadPreferenceSetting& readPref,
    const std::string& dbname,
    const BSONObj& cmdObj,
    const BSONObj& metadata,
    const ShardRegistry::ErrorCodesSet& errorsToCheck) {
    const bool isConfigShard = shard->isConfig();

    for (int retry = 1; retry <= kOnErrorNumRetries; ++retry) {
        const BSONObj cmdWithMaxTimeMS =
            (isConfigShard ? appendMaxTimeToCmdObj(txn->getRemainingMaxTimeMicros(), cmdObj)
                           : cmdObj);

        const auto swCmdResponse =
            shard->runCommand(txn, readPref, dbname, cmdWithMaxTimeMS, metadata);

        // First, check if the request failed to even reach the shard, and if we should retry.
        Status requestStatus = swCmdResponse.getStatus();
        if (!requestStatus.isOK()) {
            if (retry < kOnErrorNumRetries && errorsToCheck.count(requestStatus.code())) {
                LOG(1) << "Request " << cmdObj << " failed with retriable error and will be retried"
                       << causedBy(requestStatus);
                continue;
            } else {
                return requestStatus;
            }
        }

        // If the request reached the shard, we might return the command response or an error
        // status.
        const auto cmdResponse = std::move(swCmdResponse.getValue());
        Status commandStatus = getStatusFromCommandResult(cmdResponse.response);
        Status writeConcernStatus = checkForWriteConcernError(cmdResponse.response);

        // Next, check if the command failed with a retriable error.
        if (!commandStatus.isOK() && errorsToCheck.count(commandStatus.code())) {
            if (retry < kOnErrorNumRetries) {
                // If the command failed with a retriable error and we can retry, retry.
                LOG(1) << "Command " << cmdObj << " failed with retriable error and will be retried"
                       << causedBy(commandStatus);
                continue;
            } else {
                // If the command failed with a retriable error and we can't retry, return the
                // command error as a status.
                return commandStatus;
            }
        }

        // If the command succeeded, or it failed with a non-retriable error, check if the write
        // concern failed.
        if (!writeConcernStatus.isOK()) {
            if (errorsToCheck.count(writeConcernStatus.code()) && retry < kOnErrorNumRetries) {
                // If the write concern failed with a retriable error and we can retry, retry.
                LOG(1) << "Write concern for " << cmdObj << " failed and will be retried"
                       << causedBy(writeConcernStatus);
                continue;
            } else {
                // If the write concern failed and we can't retry, return the write concern error
                // as a status.
                return writeConcernStatus;
            }
        }

        // If the command succeeded, or if it failed with a non-retriable error but the write
        // concern was ok, return the command response object.
        return cmdResponse;
    }
    MONGO_UNREACHABLE;
}

}  // namespace mongo
