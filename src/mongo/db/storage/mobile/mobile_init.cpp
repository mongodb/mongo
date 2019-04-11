/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <sqlite3.h>

#include "mongo/base/init.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/db/storage/mobile/mobile_kv_engine.h"
#include "mongo/db/storage/mobile/mobile_options.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_options.h"

namespace mongo {

namespace {
class MobileFactory : public StorageEngine::Factory {
public:
    StorageEngine* create(const StorageGlobalParams& params,
                          const StorageEngineLockFile* lockFile) const override {
        uassert(ErrorCodes::InvalidOptions,
                "mobile does not support --groupCollections",
                !params.groupCollections);

        KVStorageEngineOptions options;
        options.directoryPerDB = params.directoryperdb;
        options.forRepair = params.repair;

        MobileKVEngine* kvEngine = new MobileKVEngine(
            params.dbpath, embedded::mobileGlobalOptions, getGlobalServiceContext());

        return new KVStorageEngine(kvEngine, options);
    }

    StringData getCanonicalName() const override {
        return "mobile";
    }

    Status validateMetadata(const StorageEngineMetadata& metadata,
                            const StorageGlobalParams& params) const override {
        return Status::OK();
    }

    BSONObj createMetadataOptions(const StorageGlobalParams& params) const override {
        return BSONObj();
    }
};

ServiceContext::ConstructorActionRegisterer mobileKVEngineInitializer{
    "MobileKVEngineInit", [](ServiceContext* service) {
        registerStorageEngine(service, std::make_unique<MobileFactory>());
    }};
}  // namespace
}  // namespace mongo
