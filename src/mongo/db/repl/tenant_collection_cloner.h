/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#pragma once

#include <memory>
#include <vector>

#include "mongo/db/repl/base_cloner.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/util/progress_meter.h"

namespace mongo {
namespace repl {

class TenantCollectionCloner : public BaseCloner {
public:
    struct Stats {
        static constexpr StringData kDocumentsToCopyFieldName = "documentsToCopy"_sd;
        static constexpr StringData kDocumentsCopiedFieldName = "documentsCopied"_sd;

        std::string ns;
        Date_t start;
        Date_t end;
        size_t documentToCopy{0};
        size_t documentsCopied{0};
        size_t indexes{0};
        size_t insertedBatches{0};
        size_t receivedBatches{0};

        std::string toString() const;
        BSONObj toBSON() const;
        void append(BSONObjBuilder* builder) const;
    };

    TenantCollectionCloner(const NamespaceString& ns,
                           const CollectionOptions& collectionOptions,
                           InitialSyncSharedData* sharedData,
                           const HostAndPort& source,
                           DBClientConnection* client,
                           StorageInterface* storageInterface,
                           ThreadPool* dbPool);

    virtual ~TenantCollectionCloner() = default;

    Stats getStats() const;

protected:
    ClonerStages getStages() final;

private:
    std::string describeForFuzzer(BaseClonerStage* stage) const final {
        return _sourceNss.db() + " db: { " + stage->getName() + ": UUID(\"" +
            _sourceDbAndUuid.uuid()->toString() + "\") coll: " + _sourceNss.coll() + " }";
    }

    /**
     * Temporary no-op stage.
     */
    AfterStageBehavior placeholderStage();

    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access according to class's own rules.
    // (M)  Reads and writes guarded by _mutex (defined in base class).
    // (X)  Access only allowed from the main flow of control called from run() or constructor.
    const NamespaceString _sourceNss;            // (R)
    const CollectionOptions _collectionOptions;  // (R)
    // Despite the type name, this member must always contain a UUID.
    NamespaceStringOrUUID _sourceDbAndUuid;  // (R)

    ClonerStage<TenantCollectionCloner> _placeholderStage;  // (R)

    Stats _stats;  // (M)
};

}  // namespace repl
}  // namespace mongo