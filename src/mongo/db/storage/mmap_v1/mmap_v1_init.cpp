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

#include "mongo/platform/basic.h"

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/mmap_v1/mmap_v1_engine.h"
#include "mongo/db/storage/storage_engine_metadata.h"
#include "mongo/db/storage/storage_options.h"

namespace mongo {

namespace {

class MMAPV1Factory : public StorageEngine::Factory {
public:
    virtual ~MMAPV1Factory() {}
    virtual StorageEngine* create(const StorageGlobalParams& params,
                                  const StorageEngineLockFile* lockFile) const {
        return new MMAPV1Engine(lockFile, getGlobalServiceContext()->getFastClockSource());
    }

    virtual StringData getCanonicalName() const {
        return "mmapv1";
    }

    virtual Status validateMetadata(const StorageEngineMetadata& metadata,
                                    const StorageGlobalParams& params) const {
        Status status =
            metadata.validateStorageEngineOption("directoryPerDB", params.directoryperdb);
        if (!status.isOK()) {
            return status;
        }

        return Status::OK();
    }

    virtual BSONObj createMetadataOptions(const StorageGlobalParams& params) const {
        BSONObjBuilder builder;
        builder.appendBool("directoryPerDB", params.directoryperdb);
        return builder.obj();
    }

    bool supportsReadOnly() const override {
        return true;
    }
};

}  // namespace

MONGO_INITIALIZER_WITH_PREREQUISITES(MMAPV1EngineInit, ("SetGlobalEnvironment"))
(InitializerContext* context) {
    getGlobalServiceContext()->registerStorageEngine("mmapv1", new MMAPV1Factory());
    return Status::OK();
}

}  // namespace mongo
