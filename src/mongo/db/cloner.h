// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

/**
 * copy a database (export/import basically)
 */

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_base.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/modules.h"

#include <list>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mongo {

class DBClientBase;
class NamespaceString;
class OperationContext;

class ClonerImpl {
public:
    virtual ~ClonerImpl() = default;
    virtual Status copyDb(OperationContext* opCtx,
                          const DatabaseName& dbName,
                          const std::string& masterHost,
                          const std::vector<NamespaceString>& trackedColls,
                          std::set<std::string>* clonedColls) = 0;

    virtual Status setupConn(OperationContext* opCtx, const std::string& masterHost) = 0;

    virtual StatusWith<std::vector<BSONObj>> getListOfCollections(
        OperationContext* opCtx, const DatabaseName& dbName, const std::string& masterHost) = 0;
};

class DefaultClonerImpl : public ClonerImpl {
public:
    /**
     * Copies an entire database from the specified host.
     * clonedColls: the function will return with this populated with a list of the collections that
     *              were cloned.
     * collectionsToClone: When opts.createCollections is false, this list reflects the collections
     *              that are cloned.  When opts.createCollections is true, this parameter is
     *              ignored and the collection list is fetched from the remote via _conn.
     */
    Status copyDb(OperationContext* opCtx,
                  const DatabaseName& dbName,
                  const std::string& masterHost,
                  const std::vector<NamespaceString>& trackedColls,
                  std::set<std::string>* clonedColls) override;

    Status setupConn(OperationContext* opCtx, const std::string& masterHost) override;

    StatusWith<std::vector<BSONObj>> getListOfCollections(OperationContext* opCtx,
                                                          const DatabaseName& dbName,
                                                          const std::string& masterHost) override;

private:
    std::unique_ptr<DBClientBase> _conn;

    // Filters a database's collection list and removes collections that should not be cloned.
    StatusWith<std::vector<BSONObj>> _filterCollectionsForClone(
        const DatabaseName& fromDBName, const std::list<BSONObj>& initialCollections);

    struct CreateCollectionParams {
        std::string collectionName;
        BSONObj collectionInfo;
        BSONObj idIndexSpec;
        bool trackedColls = false;
    };

    // Executes 'createCollection' for each collection described in 'createCollectionParams', in
    // 'dbName'.
    Status _createCollectionsForDb(
        OperationContext* opCtx,
        const std::vector<CreateCollectionParams>& createCollectionParams,
        const DatabaseName& dbName);

    /*
     * Returns the _id index spec from 'indexSpecs', or an empty BSONObj if none is found.
     */
    static BSONObj _getIdIndexSpec(const std::list<BSONObj>& indexSpecs);

    void _copy(OperationContext* opCtx,
               const DatabaseName& toDBName,
               const NamespaceString& nss,
               const BSONObj& from_opts,
               const BSONObj& from_id_index);

    void _copyIndexes(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const BSONObj& from_opts,
                      const std::list<BSONObj>& from_indexes);

    struct BatchHandler;

    DBClientBase* getConn() {
        return _conn.get();
    }

    /**
     * Returns true if we should use raw data operations during cloning.
     * If available, they must be used in order to clone viewless timeseries collections.
     * TODO(SERVER-101595): This method always returns true once 9.0 becomes lastLTS.
     */
    bool shouldUseRawDataOperations(const VersionContext& vCtx);
};

class [[MONGO_MOD_PUBLIC]] Cloner {

public:
    Cloner(std::unique_ptr<ClonerImpl> clonerImpl) : _clonerImpl(std::move(clonerImpl)) {}

    Cloner();

    Cloner(const Cloner&) = delete;

    Cloner& operator=(const Cloner&) = delete;

    Status copyDb(OperationContext* opCtx,
                  const DatabaseName& dbName,
                  const std::string& masterHost,
                  const std::vector<NamespaceString>& trackedColls,
                  std::set<std::string>* clonedColls);

    StatusWith<std::vector<BSONObj>> getListOfCollections(OperationContext* opCtx,
                                                          const DatabaseName& dbName,
                                                          const std::string& masterHost);

private:
    std::unique_ptr<ClonerImpl> _clonerImpl;
};

}  // namespace mongo
