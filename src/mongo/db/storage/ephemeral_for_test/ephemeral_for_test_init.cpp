// ephemeral_for_test_init.cpp

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

#include "mongo/base/init.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/ephemeral_for_test/ephemeral_for_test_engine.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/db/storage/storage_options.h"

namespace mongo {

namespace {

class EphemeralForTestFactory : public StorageEngine::Factory {
public:
    virtual ~EphemeralForTestFactory() {}
    virtual StorageEngine* create(const StorageGlobalParams& params,
                                  const StorageEngineLockFile* lockFile) const {
        KVStorageEngineOptions options;
        options.directoryPerDB = params.directoryperdb;
        options.forRepair = params.repair;
        return new KVStorageEngine(new EphemeralForTestEngine(), options);
    }

    virtual StringData getCanonicalName() const {
        return "ephemeralForTest";
    }

    virtual Status validateMetadata(const StorageEngineMetadata& metadata,
                                    const StorageGlobalParams& params) const {
        return Status::OK();
    }

    virtual BSONObj createMetadataOptions(const StorageGlobalParams& params) const {
        return BSONObj();
    }
};

}  // namespace

MONGO_INITIALIZER_WITH_PREREQUISITES(EphemeralForTestEngineInit, ("SetGlobalEnvironment"))
(InitializerContext* context) {
    getGlobalServiceContext()->registerStorageEngine("ephemeralForTest",
                                                     new EphemeralForTestFactory());
    return Status::OK();
}

}  // namespace mongo
