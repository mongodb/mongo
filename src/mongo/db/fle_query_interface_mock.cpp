/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/fle_query_interface_mock.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <limits>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

BSONObj FLEQueryInterfaceMock::getById(const NamespaceString& nss, BSONElement element) {
    auto obj = BSON("_id" << element);
    auto swDoc = _storage->findById(_opCtx, nss, obj.firstElement());
    if (swDoc.getStatus() == ErrorCodes::NoSuchKey) {
        return BSONObj();
    }

    return uassertStatusOK(swDoc);
}

BSONObj FLEQueryInterfaceMock::getById(const NamespaceString& nss, PrfBlock block) {
    auto doc = BSON("v" << BSONBinData(block.data(), block.size(), BinDataGeneral));
    BSONElement element = doc.firstElement();
    return getById(nss, element);
}

uint64_t FLEQueryInterfaceMock::countDocuments(const NamespaceString& nss) {
    return uassertStatusOK(_storage->getCollectionCount(_opCtx, nss));
}

std::vector<std::vector<FLEEdgeCountInfo>> FLEQueryInterfaceMock::getTags(
    const NamespaceString& nss,
    const std::vector<std::vector<FLEEdgePrfBlock>>& tokensSets,
    FLETagQueryInterface::TagQueryType type) {

    return getTagsFromStorage(_opCtx, nss, tokensSets, type);
}

StatusWith<write_ops::InsertCommandReply> FLEQueryInterfaceMock::insertDocuments(
    const NamespaceString& nss,
    std::vector<BSONObj> objs,
    StmtId* pStmtId,
    bool translateDuplicateKey,
    bool bypassDocumentValidation) {

    for (auto& obj : objs) {
        repl::TimestampedBSONObj tb;
        tb.obj = obj;

        auto status = _storage->insertDocument(_opCtx, nss, tb, 0);

        if (!status.isOK()) {
            return status;
        }
    }

    return write_ops::InsertCommandReply();
}

std::pair<write_ops::DeleteCommandReply, BSONObj> FLEQueryInterfaceMock::deleteWithPreimage(
    const NamespaceString& nss,
    const EncryptionInformation& ei,
    const write_ops::DeleteCommandRequest& deleteRequest) {
    // A limit of the API, we can delete by _id and get the pre-image so we limit our unittests to
    // this
    uassert(6346808,
            "Delete command must have exactly one delete op entry",
            deleteRequest.getDeletes().size() == 1);

    auto deleteOpEntry = deleteRequest.getDeletes()[0];

    uassert(6346809,
            "First element field name of delete op entry must be '_id'",
            "_id"_sd == deleteOpEntry.getQ().firstElementFieldNameStringData());

    BSONElement id = deleteOpEntry.getQ().firstElement();
    if (id.isABSONObj() && id.Obj().firstElementFieldNameStringData() == "$eq"_sd) {
        id = id.Obj().firstElement();
    }

    auto swDoc = _storage->deleteById(_opCtx, nss, id);

    // Some of the unit tests delete documents that do not exist
    if (swDoc.getStatus() == ErrorCodes::NoSuchKey) {
        return {write_ops::DeleteCommandReply(), BSONObj()};
    }

    return {write_ops::DeleteCommandReply(), uassertStatusOK(swDoc)};
}

write_ops::DeleteCommandReply FLEQueryInterfaceMock::deleteDocument(
    const NamespaceString& nss, int32_t stmtId, write_ops::DeleteCommandRequest& deleteRequest) {
    return deleteWithPreimage(nss, {}, deleteRequest).first;
}

std::pair<write_ops::UpdateCommandReply, BSONObj> FLEQueryInterfaceMock::updateWithPreimage(
    const NamespaceString& nss,
    const EncryptionInformation& ei,
    const write_ops::UpdateCommandRequest& updateRequest) {
    // A limit of the API, we can delete by _id and get the pre-image so we limit our unittests to
    // this
    uassert(6346810,
            "Update command must have exactly one update op entry",
            updateRequest.getUpdates().size() == 1);

    auto updateOpEntry = updateRequest.getUpdates()[0];

    uassert(6346811,
            "First element field name of update op entry must be '_id'",
            "_id"_sd == updateOpEntry.getQ().firstElementFieldNameStringData());

    BSONElement id = updateOpEntry.getQ().firstElement();
    if (id.isABSONObj() && id.Obj().firstElementFieldNameStringData() == "$eq"_sd) {
        id = id.Obj().firstElement();
    }
    BSONObj preimage = getById(nss, id);

    if (updateOpEntry.getU().type() == write_ops::UpdateModification::Type::kModifier) {
        uassertStatusOK(
            _storage->upsertById(_opCtx, nss, id, updateOpEntry.getU().getUpdateModifier()));
    } else {
        uassertStatusOK(
            _storage->upsertById(_opCtx, nss, id, updateOpEntry.getU().getUpdateReplacement()));
    }

    return {write_ops::UpdateCommandReply(), preimage};
}

write_ops::UpdateCommandReply FLEQueryInterfaceMock::update(
    const NamespaceString& nss, int32_t stmtId, write_ops::UpdateCommandRequest& updateRequest) {
    auto [reply, _] = updateWithPreimage(nss, EncryptionInformation(), updateRequest);
    return reply;
}

write_ops::FindAndModifyCommandReply FLEQueryInterfaceMock::findAndModify(
    const NamespaceString& nss,
    const EncryptionInformation& ei,
    const write_ops::FindAndModifyCommandRequest& findAndModifyRequest) {
    // Repl storage interface does not have find and modify support directly. We emulate it, poorly
    uassert(6346812,
            "First element field name of findAndModify query must be '_id'",
            "_id"_sd == findAndModifyRequest.getQuery().firstElementFieldNameStringData());
    uassert(6346813,
            "findAndModify 'new' field must be 'false'",
            findAndModifyRequest.getNew().get_value_or(false) == false);

    // The query may be the short form {_id: 1} or the long form {_id: {$eq: 1}}.
    auto idElt = [&]() {
        auto id = findAndModifyRequest.getQuery().firstElement();
        if (id.type() == BSONType::object && id.Obj().hasField("$eq")) {
            return id.Obj()["$eq"];
        }
        return id;
    }();
    BSONObj preimage = getById(nss, idElt);

    if (findAndModifyRequest.getRemove().get_value_or(false)) {
        // Remove
        auto swDoc = _storage->deleteById(_opCtx, nss, idElt);
        uassertStatusOK(swDoc);

    } else {
        uassertStatusOK(_storage->upsertById(
            _opCtx, nss, idElt, findAndModifyRequest.getUpdate()->getUpdateModifier()));
    }

    write_ops::FindAndModifyCommandReply reply;
    reply.setValue(preimage);
    return reply;
}

std::vector<BSONObj> FLEQueryInterfaceMock::findDocuments(const NamespaceString& nss,
                                                          BSONObj filter) {
    std::vector<BSONObj> results;
    auto docs =
        uassertStatusOK(_storage->findDocuments(_opCtx,
                                                nss,
                                                boost::none,
                                                repl::StorageInterface::ScanDirection::kForward,
                                                {},
                                                BoundInclusion::kIncludeStartKeyOnly,
                                                std::numeric_limits<size_t>::max()));
    for (auto& doc : docs) {
        auto elt = doc.getField(filter.firstElementFieldNameStringData());
        if (elt.binaryEqual(filter.firstElement())) {
            results.push_back(doc.getOwned());
        }
    }
    return results;
}

}  // namespace mongo
