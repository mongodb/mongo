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

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/db/storage/storage_engine_metadata.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_parameters.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_server_status.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
class WiredTigerFactory : public StorageEngine::Factory {
public:
    virtual ~WiredTigerFactory() {}
    virtual StorageEngine* create(const StorageGlobalParams& params,
                                  const StorageEngineLockFile* lockFile) const {
        if (lockFile && lockFile->createdByUncleanShutdown()) {
            warning() << "Recovering data from the last clean checkpoint.";
        }

        size_t cacheMB = WiredTigerUtil::getCacheSizeMB(wiredTigerGlobalOptions.cacheSizeGB);
        const bool ephemeral = false;
        WiredTigerKVEngine* kv =
            new WiredTigerKVEngine(getCanonicalName().toString(),
                                   params.dbpath,
                                   getGlobalServiceContext()->getFastClockSource(),
                                   wiredTigerGlobalOptions.engineConfig,
                                   cacheMB,
                                   params.dur,
                                   ephemeral,
                                   params.repair,
                                   params.readOnly);
        kv->setRecordStoreExtraOptions(wiredTigerGlobalOptions.collectionConfig);
        kv->setSortedDataInterfaceExtraOptions(wiredTigerGlobalOptions.indexConfig);
        // Intentionally leaked.
        new WiredTigerServerStatusSection(kv);
        new WiredTigerEngineRuntimeConfigParameter(kv);

        KVStorageEngineOptions options;
        options.directoryPerDB = params.directoryperdb;
        options.directoryForIndexes = wiredTigerGlobalOptions.directoryForIndexes;
        options.forRepair = params.repair;
        return new KVStorageEngine(kv, options);
    }

    virtual StringData getCanonicalName() const {
        return kWiredTigerEngineName;
    }

    virtual Status validateCollectionStorageOptions(const BSONObj& options) const {
        return WiredTigerRecordStore::parseOptionsField(options).getStatus();
    }

    virtual Status validateIndexStorageOptions(const BSONObj& options) const {
        return WiredTigerIndex::parseIndexOptions(options).getStatus();
    }

    virtual Status validateMetadata(const StorageEngineMetadata& metadata,
                                    const StorageGlobalParams& params) const {
        Status status =
            metadata.validateStorageEngineOption("directoryPerDB", params.directoryperdb);
        if (!status.isOK()) {
            return status;
        }

        status = metadata.validateStorageEngineOption("directoryForIndexes",
                                                      wiredTigerGlobalOptions.directoryForIndexes);
        if (!status.isOK()) {
            return status;
        }

        return Status::OK();
    }

    virtual BSONObj createMetadataOptions(const StorageGlobalParams& params) const {
        BSONObjBuilder builder;
        builder.appendBool("directoryPerDB", params.directoryperdb);
        builder.appendBool("directoryForIndexes", wiredTigerGlobalOptions.directoryForIndexes);
        return builder.obj();
    }

    bool supportsReadOnly() const final {
        return true;
    }
};
}  // namespace

MONGO_INITIALIZER_WITH_PREREQUISITES(WiredTigerEngineInit, ("SetGlobalEnvironment"))
(InitializerContext* context) {
    getGlobalServiceContext()->registerStorageEngine(kWiredTigerEngineName,
                                                     new WiredTigerFactory());

    return Status::OK();
}
}
