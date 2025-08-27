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

#include "mongo/db/query/write_ops/insert.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_depth.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/local_catalog/document_validation.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/mongod_options_storage_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/util/validate_id.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/update/storage_validation.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

/**
 * Validates the nesting depth of 'obj', returning a non-OK status if it exceeds the limit.
 */
Status validateDepth(const BSONObj& obj) {
    std::vector<BSONObjIterator> frames;
    frames.reserve(16);
    frames.emplace_back(obj);

    while (!frames.empty()) {
        const auto elem = frames.back().next();
        if (elem.type() == BSONType::object || elem.type() == BSONType::array) {
            auto subObj = elem.embeddedObject();
            // Empty subdocuments do not count toward the depth of a document.
            if (MONGO_unlikely(frames.size() == BSONDepth::getMaxDepthForUserStorage() &&
                               !subObj.isEmpty())) {
                // We're exactly at the limit, so descending to the next level would exceed
                // the maximum depth.
                return {ErrorCodes::Overflow,
                        str::stream()
                            << "cannot insert document because it exceeds "
                            << BSONDepth::getMaxDepthForUserStorage() << " levels of nesting"};
            }
            frames.emplace_back(subObj);
        }

        if (!frames.back().more()) {
            frames.pop_back();
        }
    }

    return Status::OK();
}
}  // namespace

StatusWith<BSONObj> fixDocumentForInsert(OperationContext* opCtx,
                                         const BSONObj& doc,
                                         bool bypassEmptyTsReplacement,
                                         bool* containsDotsAndDollarsField) {
    bool validationDisabled = DocumentValidationSettings::get(opCtx).isInternalValidationDisabled();

    if (!validationDisabled) {
        // 'gAllowDocumentsGreaterThanMaxUserSize' should only ever be enabled when restoring a node
        // from a backup. For some restores, we re-insert whole oplog entries from a
        // source cluster to a destination cluster. Some generated oplog entries may exceed the user
        // maximum due to entry metadata, and therefore we should skip BSON size validation for
        // these inserts. Note that we should only skip the size check when inserting oplog entries
        // into the oplog and not when inserting user documents. The oplog entries to insert have
        // already been validated for size on the source cluster, and were successfully inserted
        // into the source oplog.
        if (doc.objsize() > BSONObjMaxUserSize && !gAllowDocumentsGreaterThanMaxUserSize)
            return StatusWith<BSONObj>(ErrorCodes::BSONObjectTooLarge,
                                       str::stream() << "object to insert too large"
                                                     << ". size in bytes: " << doc.objsize()
                                                     << ", max size: " << BSONObjMaxUserSize);

        auto depthStatus = validateDepth(doc);
        if (!depthStatus.isOK()) {
            return depthStatus;
        }
    }

    bool firstElementIsId = false;
    bool hasTimestampToFix = false;
    bool hadId = false;
    {
        BSONObjIterator i(doc);
        for (bool isFirstElement = true; i.more(); isFirstElement = false) {
            BSONElement e = i.next();

            auto fieldName = e.fieldNameStringData();
            if (fieldName.starts_with('$') && containsDotsAndDollarsField) {
                *containsDotsAndDollarsField = true;
                // If the internal validation is disabled and we confirm this doc contains
                // dots/dollars field name, we can skip other validations below.
                if (validationDisabled) {
                    return StatusWith<BSONObj>(BSONObj());
                }
            }

            if (!validationDisabled) {
                if (!bypassEmptyTsReplacement && e.type() == BSONType::timestamp &&
                    e.timestampValue() == 0) {
                    // we replace Timestamp(0,0) at the top level with a correct value
                    // in the fast pass, we just mark that we want to swap
                    hasTimestampToFix = true;
                }

                // check no regexp for _id (SERVER-9502)
                // also, disallow undefined and arrays
                // Make sure _id isn't duplicated (SERVER-19361).
                if (fieldName == "_id") {
                    auto status = validIdField(e);
                    if (!status.isOK()) {
                        return StatusWith<BSONObj>(status);
                    }
                    if (hadId) {
                        return StatusWith<BSONObj>(
                            ErrorCodes::BadValue, "can't have multiple _id fields in one document");
                    } else {
                        hadId = true;
                        firstElementIsId = isFirstElement;
                    }
                }
            }
        }
    }

    if (validationDisabled || (firstElementIsId && !hasTimestampToFix))
        return StatusWith<BSONObj>(BSONObj());

    BSONObjIterator i(doc);

    BSONObjBuilder b(doc.objsize() + 16);
    if (firstElementIsId) {
        b.append(doc.firstElement());
        i.next();
    } else {
        BSONElement e = doc["_id"];
        if (stdx::to_underlying(e.type())) {
            b.append(e);
        } else {
            b.appendOID("_id", nullptr, true);
        }
    }

    while (i.more()) {
        BSONElement e = i.next();
        if (hadId && e.fieldNameStringData() == "_id") {
            // no-op
        } else if (!bypassEmptyTsReplacement && e.type() == BSONType::timestamp &&
                   e.timestampValue() == 0) {
            auto nextTime = VectorClockMutable::get(opCtx)->tickClusterTime(1);
            b.append(e.fieldName(), nextTime.asTimestamp());
        } else {
            b.append(e);
        }
    }
    return StatusWith<BSONObj>(b.obj());
}

Status userAllowedWriteNS(OperationContext* opCtx, const NamespaceString& ns) {
    if (!opCtx->isEnforcingConstraints()) {
        // Mechanisms like oplog application call into `userAllowedCreateNS`. Relax constraints for
        // those circumstances.
        return Status::OK();
    }

    if (ns.isSystemDotProfile() || ns.isSystemDotViews() ||
        (ns.isOplog() &&
         repl::ReplicationCoordinator::get(getGlobalServiceContext())->getSettings().isReplSet())) {
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "cannot write to " << ns.toStringForErrorMsg());
    }
    return userAllowedCreateNS(opCtx, ns);
}

Status userAllowedCreateNS(OperationContext* opCtx, const NamespaceString& ns) {
    if (!opCtx->isEnforcingConstraints()) {
        // Mechanisms like oplog application call into `userAllowedCreateNS`. Relax constraints for
        // those circumstances.
        return Status::OK();
    }

    if (!ns.isValid(DatabaseName::DollarInDbNameBehavior::Disallow)) {
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "Invalid namespace: " << ns.toStringForErrorMsg());
    }

    if (!NamespaceString::validCollectionName(ns.coll())) {
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "Invalid collection name: " << ns.coll());
    }

    if (ns.isSystemDotProfile()) {
        return Status::OK();
    }

    if (ns.isSystem() && !ns.isLegalClientSystemNS()) {
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "Invalid system namespace: " << ns.toStringForErrorMsg());
    }

    auto maxNsLen = ns.isOnInternalDb() ? NamespaceString::MaxInternalNsCollectionLen
                                        : NamespaceString::MaxUserNsCollectionLen;
    if (ns.isNormalCollection() && ns.size() > maxNsLen) {
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "Fully qualified namespace is too long. Namespace: "
                                    << ns.toStringForErrorMsg() << " Max: " << maxNsLen);
    }

    if (ns.coll().find(".system.") != std::string::npos) {
        // Writes are permitted to the persisted chunk metadata collections. These collections are
        // named based on the name of the sharded collection, e.g.
        // 'config.cache.chunks.dbname.collname/colluuid'. Since there is a sharded collection
        // 'config.system.sessions', there will be a corresponding persisted chunk metadata
        // collection 'config.cache.chunks.config.system.sessions'. We wish to allow writes to this
        // collection.
        if (ns.isConfigDotCacheDotChunks()) {
            return Status::OK();
        }

        if (ns.isConfigDB() && ns.isLegalClientSystemNS()) {
            return Status::OK();
        }

        return Status(ErrorCodes::BadValue,
                      str::stream() << "Invalid namespace: " << ns.toStringForErrorMsg());
    }

    return Status::OK();
}
}  // namespace mongo
