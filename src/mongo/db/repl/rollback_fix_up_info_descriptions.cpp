/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/rollback_fix_up_info_descriptions.h"

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace repl {

namespace {

/**
 * Appends single document op type to builder as string element under the field name "op".
 */
void appendOpTypeToBuilder(RollbackFixUpInfo::SingleDocumentOpType opType,
                           BSONObjBuilder* builder) {
    auto fieldName = "operationType"_sd;
    switch (opType) {
        case RollbackFixUpInfo::SingleDocumentOpType::kDelete:
            builder->append(fieldName, "delete");
            break;
        case RollbackFixUpInfo::SingleDocumentOpType::kInsert:
            builder->append(fieldName, "insert");
            break;
        case RollbackFixUpInfo::SingleDocumentOpType::kUpdate:
            builder->append(fieldName, "update");
            break;
    }
}

/**
 * Returns string representation of RollbackFixUpInfo::IndexOpType.
 */
std::string toString(RollbackFixUpInfo::IndexOpType opType) {
    switch (opType) {
        case RollbackFixUpInfo::IndexOpType::kCreate:
            return "create";
        case RollbackFixUpInfo::IndexOpType::kDrop:
            return "drop";
        case RollbackFixUpInfo::IndexOpType::kUpdateTTL:
            return "updateTTL";
    }
    MONGO_UNREACHABLE;
}
/**
 * Appends index op type to builder as string element under the field name "op".
 */
void appendOpTypeToBuilder(RollbackFixUpInfo::IndexOpType opType, BSONObjBuilder* builder) {
    builder->append("operationType", toString(opType));
}

}  // namespace

RollbackFixUpInfo::SingleDocumentOperationDescription::SingleDocumentOperationDescription(
    const UUID& collectionUuid,
    const BSONElement& docId,
    RollbackFixUpInfo::SingleDocumentOpType opType,
    const std::string& dbName)
    : _collectionUuid(collectionUuid),
      _wrappedDocId(docId.wrap("documentId")),
      _opType(opType),
      _dbName(dbName) {}

BSONObj RollbackFixUpInfo::SingleDocumentOperationDescription::toBSON() const {
    // For non-insert operations, we will use the collection UUID and document id to query the sync
    // source (using the source collection's default collation) for the most recent copy of the
    // deleted/updated document. This is necessary to complete the rollback for a deleted/updated
    // document.
    // Insert operations are rolled back by deleting the the document from the local collection.
    BSONObjBuilder bob;
    {
        BSONObjBuilder idBob(bob.subobjStart("_id"));
        _collectionUuid.appendToBuilder(&idBob, "collectionUuid");
        idBob.append(_wrappedDocId.firstElement());
    }

    // This matches the "op" field in the oplog entry.
    appendOpTypeToBuilder(_opType, &bob);

    // The database name is used in the find command request when fetching the document from the
    // sync source.
    bob.append("db", _dbName);

    // This will be replaced by the most recent copy of the affected document from the sync
    // source. If the document is not found on the sync source, this will remain null.
    bob.appendNull("documentToRestore");

    return bob.obj();
}

RollbackFixUpInfo::CollectionUuidDescription::CollectionUuidDescription(const UUID& collectionUuid,
                                                                        const NamespaceString& nss)
    : _collectionUuid(collectionUuid), _nss(nss) {}

BSONObj RollbackFixUpInfo::CollectionUuidDescription::toBSON() const {
    BSONObjBuilder bob;
    _collectionUuid.appendToBuilder(&bob, "_id");
    bob.append("ns", _nss.ns());
    return bob.obj();
}

RollbackFixUpInfo::CollectionOptionsDescription::CollectionOptionsDescription(
    const UUID& collectionUuid, const BSONObj& optionsObj)
    : _collectionUuid(collectionUuid), _optionsObj(optionsObj) {}

BSONObj RollbackFixUpInfo::CollectionOptionsDescription::toBSON() const {
    BSONObjBuilder bob;
    _collectionUuid.appendToBuilder(&bob, "_id");
    bob.append("options", _optionsObj);
    return bob.obj();
}

RollbackFixUpInfo::IndexDescription::IndexDescription(const UUID& collectionUuid,
                                                      const std::string& indexName,
                                                      RollbackFixUpInfo::IndexOpType opType,
                                                      const BSONObj& infoObj)
    : _collectionUuid(collectionUuid), _indexName(indexName), _opType(opType), _infoObj(infoObj) {
    invariant(RollbackFixUpInfo::IndexOpType::kUpdateTTL != _opType);
}

RollbackFixUpInfo::IndexDescription::IndexDescription(const UUID& collectionUuid,
                                                      const std::string& indexName,
                                                      Seconds expireAfterSeconds)
    : _collectionUuid(collectionUuid),
      _indexName(indexName),
      _opType(RollbackFixUpInfo::IndexOpType::kUpdateTTL),
      _expireAfterSeconds(expireAfterSeconds) {
    BSONObjBuilder bob;
    bob.append("expireAfterSeconds", durationCount<Seconds>(*_expireAfterSeconds));
    _infoObj = bob.obj();
}

RollbackFixUpInfo::IndexOpType RollbackFixUpInfo::IndexDescription::getOpType() const {
    return _opType;
}

std::string RollbackFixUpInfo::IndexDescription::getOpTypeAsString() const {
    return toString(_opType);
}

boost::optional<Seconds> RollbackFixUpInfo::IndexDescription::getExpireAfterSeconds() const {
    return _expireAfterSeconds;
}

// static
StatusWith<RollbackFixUpInfo::IndexOpType> RollbackFixUpInfo::IndexDescription::parseOpType(
    const BSONObj& doc) {
    std::string opTypeStr;
    auto status = bsonExtractStringField(doc, "operationType"_sd, &opTypeStr);
    if (!status.isOK()) {
        return status;
    }
    if ("create" == opTypeStr) {
        return RollbackFixUpInfo::IndexOpType::kCreate;
    } else if ("drop" == opTypeStr) {
        return RollbackFixUpInfo::IndexOpType::kDrop;
    } else if ("updateTTL" == opTypeStr) {
        return RollbackFixUpInfo::IndexOpType::kUpdateTTL;
    }
    return Status(ErrorCodes::FailedToParse,
                  str::stream() << "Unrecognized RollbackFixUpInfo::IndexOpType: " << opTypeStr);
}

BSONObj RollbackFixUpInfo::IndexDescription::toBSON() const {
    BSONObjBuilder bob;
    bob.append("_id", makeIdKey());
    appendOpTypeToBuilder(_opType, &bob);
    bob.append("infoObj", _infoObj);

    return bob.obj();
}

BSONObj RollbackFixUpInfo::IndexDescription::makeIdKey() const {
    BSONObjBuilder idBob;
    _collectionUuid.appendToBuilder(&idBob, "collectionUuid");
    idBob.append("indexName", _indexName);
    return idBob.obj();
}

}  // namespace repl

std::ostream& operator<<(std::ostream& os, const repl::RollbackFixUpInfo::IndexOpType& opType) {
    return os << repl::toString(opType);
}

}  // namespace mongo
