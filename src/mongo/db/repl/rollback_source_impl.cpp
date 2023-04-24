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

#include "mongo/db/repl/rollback_source_impl.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/replication_auth.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {
namespace repl {

RollbackSourceImpl::RollbackSourceImpl(GetConnectionFn getConnection,
                                       const HostAndPort& source,
                                       StringData collectionName,
                                       int batchSize)
    : _getConnection(getConnection),
      _source(source),
      _collectionName(collectionName),
      _oplog(source, getConnection, collectionName, batchSize) {}

const OplogInterface& RollbackSourceImpl::getOplog() const {
    return _oplog;
}

const HostAndPort& RollbackSourceImpl::getSource() const {
    return _source;
}


int RollbackSourceImpl::getRollbackId() const {
    bo info;
    _getConnection()->runCommand(DatabaseName::kAdmin, BSON("replSetGetRBID" << 1), info);
    return info["rbid"].numberInt();
}

BSONObj RollbackSourceImpl::getLastOperation() const {
    FindCommandRequest findCmd{NamespaceString{_collectionName}};
    findCmd.setSort(BSON("$natural" << -1));
    findCmd.setReadConcern(ReadConcernArgs::kLocal);
    return _getConnection()->findOne(std::move(findCmd),
                                     ReadPreferenceSetting{ReadPreference::SecondaryPreferred});
}

BSONObj RollbackSourceImpl::findOne(const NamespaceString& nss, const BSONObj& filter) const {
    FindCommandRequest findCmd{nss};
    findCmd.setFilter(filter);
    findCmd.setReadConcern(ReadConcernArgs::kLocal);
    return _getConnection()
        ->findOne(std::move(findCmd), ReadPreferenceSetting{ReadPreference::SecondaryPreferred})
        .getOwned();
}

std::pair<BSONObj, NamespaceString> RollbackSourceImpl::findOneByUUID(const std::string& db,
                                                                      UUID uuid,
                                                                      const BSONObj& filter) const {
    FindCommandRequest findRequest{NamespaceStringOrUUID{db, uuid}};
    findRequest.setFilter(filter);
    findRequest.setReadConcern(ReadConcernArgs::kLocal);
    findRequest.setLimit(1);
    findRequest.setSingleBatch(true);

    auto cursor =
        std::make_unique<DBClientCursor>(_getConnection(),
                                         std::move(findRequest),
                                         ReadPreferenceSetting{ReadPreference::SecondaryPreferred},
                                         false /*isExhaust*/);
    uassert(6138500, "find one by UUID failed", cursor->init());
    BSONObj result = cursor->more() ? cursor->nextSafe() : BSONObj{};
    NamespaceString nss = cursor->getNamespaceString();
    return {std::move(result), std::move(nss)};
}

StatusWith<BSONObj> RollbackSourceImpl::getCollectionInfoByUUID(const DatabaseName& dbName,
                                                                const UUID& uuid) const {
    std::list<BSONObj> info =
        _getConnection()->getCollectionInfos(dbName, BSON("info.uuid" << uuid));
    if (info.empty()) {
        return StatusWith<BSONObj>(
            ErrorCodes::NoSuchKey,
            str::stream() << "No collection info found for collection with uuid: "
                          << uuid.toString() << " in db: " << dbName.toStringForErrorMsg());
    }
    invariant(info.size() == 1U);
    return info.front();
}

StatusWith<BSONObj> RollbackSourceImpl::getCollectionInfo(const NamespaceString& nss) const {
    std::list<BSONObj> info =
        _getConnection()->getCollectionInfos(nss.dbName(), BSON("name" << nss.coll()));
    if (info.empty()) {
        return StatusWith<BSONObj>(ErrorCodes::NoSuchKey,
                                   str::stream() << "no collection info found: "
                                                 << nss.toStringForErrorMsg());
    }
    invariant(info.size() == 1U);
    return info.front();
}

}  // namespace repl
}  // namespace mongo
