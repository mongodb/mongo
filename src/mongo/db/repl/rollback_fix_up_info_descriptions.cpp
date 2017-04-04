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

#include "mongo/db/jsobj.h"

namespace mongo {
namespace repl {

namespace {

/**
 * Appends op type to builder as string element under the field name "op".
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

}  // namespace

RollbackFixUpInfo::SingleDocumentOperationDescription::SingleDocumentOperationDescription(
    const UUID& collectionUuid,
    const BSONElement& docId,
    RollbackFixUpInfo::SingleDocumentOpType opType)
    : _collectionUuid(collectionUuid), _wrappedDocId(docId.wrap("documentId")), _opType(opType) {}

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

}  // namespace repl
}  // namespace mongo
