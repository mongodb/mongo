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

#pragma once

/**
 * copy a database (export/import basically)
 */

#include <string>
#include <vector>

#include "mongo/client/connpool.h"
#include "mongo/client/dbclient_base.h"

namespace mongo {

class DBClientBase;
class NamespaceString;
class OperationContext;

class ClonerImpl {
public:
    virtual ~ClonerImpl() = default;
    virtual Status copyDb(OperationContext* opCtx,
                          const std::string& dBName,
                          const std::string& masterHost,
                          const std::vector<NamespaceString>& shardedColls,
                          std::set<std::string>* clonedColls) = 0;

    virtual Status setupConn(OperationContext* opCtx,
                             const std::string& dBName,
                             const std::string& masterHost) = 0;

    virtual StatusWith<std::vector<BSONObj>> getListOfCollections(
        OperationContext* opCtx, const std::string& dBName, const std::string& masterHost) = 0;
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
                  const std::string& dBName,
                  const std::string& masterHost,
                  const std::vector<NamespaceString>& shardedColls,
                  std::set<std::string>* clonedColls) override;

    Status setupConn(OperationContext* opCtx,
                     const std::string& dBName,
                     const std::string& masterHost) override;

    StatusWith<std::vector<BSONObj>> getListOfCollections(OperationContext* opCtx,
                                                          const std::string& dBName,
                                                          const std::string& masterHost) override;

private:
    std::unique_ptr<ScopedDbConnection> _conn;

    // Filters a database's collection list and removes collections that should not be cloned.
    StatusWith<std::vector<BSONObj>> _filterCollectionsForClone(
        const std::string& fromDBName, const std::list<BSONObj>& initialCollections);

    struct CreateCollectionParams {
        std::string collectionName;
        BSONObj collectionInfo;
        BSONObj idIndexSpec;
        bool shardedColl = false;
    };

    // Executes 'createCollection' for each collection described in 'createCollectionParams', in
    // 'dbName'.
    Status _createCollectionsForDb(
        OperationContext* opCtx,
        const std::vector<CreateCollectionParams>& createCollectionParams,
        const std::string& dbName);

    /*
     * Returns the _id index spec from 'indexSpecs', or an empty BSONObj if none is found.
     */
    static BSONObj _getIdIndexSpec(const std::list<BSONObj>& indexSpecs);

    void _copy(OperationContext* opCtx,
               const std::string& toDBName,
               const NamespaceString& nss,
               const BSONObj& from_opts,
               const BSONObj& from_id_index);

    void _copyIndexes(OperationContext* opCtx,
                      const std::string& toDBName,
                      const NamespaceString& nss,
                      const BSONObj& from_opts,
                      const std::list<BSONObj>& from_indexes);

    struct BatchHandler;

    DBClientBase* getConn() {
        return _conn->get();
    }
};

class Cloner {

public:
    Cloner(std::unique_ptr<ClonerImpl> clonerImpl) : _clonerImpl(std::move(clonerImpl)) {}

    Cloner();

    Cloner(const Cloner&) = delete;

    Cloner& operator=(const Cloner&) = delete;

    Status copyDb(OperationContext* opCtx,
                  const std::string& dBName,
                  const std::string& masterHost,
                  const std::vector<NamespaceString>& shardedColls,
                  std::set<std::string>* clonedColls);

    StatusWith<std::vector<BSONObj>> getListOfCollections(OperationContext* opCtx,
                                                          const std::string& dBName,
                                                          const std::string& masterHost);

private:
    std::unique_ptr<ClonerImpl> _clonerImpl;
};

}  // namespace mongo
