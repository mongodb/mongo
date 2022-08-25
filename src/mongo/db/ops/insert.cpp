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

#include "mongo/db/ops/insert.h"

#include <vector>

#include "mongo/bson/bson_depth.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/query/dbref.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/update/storage_validation.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/db/views/durable_view_catalog.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

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
        if (elem.type() == BSONType::Object || elem.type() == BSONType::Array) {
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
                                         bool* containsDotsAndDollarsField) {
    bool validationDisabled = DocumentValidationSettings::get(opCtx).isInternalValidationDisabled();

    if (!validationDisabled) {
        if (doc.objsize() > BSONObjMaxUserSize)
            return StatusWith<BSONObj>(ErrorCodes::BadValue,
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

            if (fieldName[0] == '$' && containsDotsAndDollarsField) {
                *containsDotsAndDollarsField = true;
                // If the internal validation is disabled and we confirm this doc contains
                // dots/dollars field name, we can skip other validations below.
                if (validationDisabled) {
                    return StatusWith<BSONObj>(BSONObj());
                }
            }

            if (!validationDisabled) {
                if (e.type() == bsonTimestamp && e.timestampValue() == 0) {
                    // we replace Timestamp(0,0) at the top level with a correct value
                    // in the fast pass, we just mark that we want to swap
                    hasTimestampToFix = true;
                }

                // check no regexp for _id (SERVER-9502)
                // also, disallow undefined and arrays
                // Make sure _id isn't duplicated (SERVER-19361).
                if (fieldName == "_id") {
                    auto status = storage_validation::storageValidIdField(e);
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
        if (e.type()) {
            b.append(e);
        } else {
            b.appendOID("_id", nullptr, true);
        }
    }

    while (i.more()) {
        BSONElement e = i.next();
        if (hadId && e.fieldNameStringData() == "_id") {
            // no-op
        } else if (e.type() == bsonTimestamp && e.timestampValue() == 0) {
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
         repl::ReplicationCoordinator::get(getGlobalServiceContext())->isReplEnabled())) {
        return Status(ErrorCodes::InvalidNamespace, str::stream() << "cannot write to " << ns);
    }
    return userAllowedCreateNS(opCtx, ns);
}

Status userAllowedCreateNS(OperationContext* opCtx, const NamespaceString& ns) {
    if (!opCtx->isEnforcingConstraints()) {
        // Mechanisms like oplog application call into `userAllowedCreateNS`. Relax constraints for
        // those circumstances.
        return Status::OK();
    }

    if (!ns.isValid(NamespaceString::DollarInDbNameBehavior::Disallow)) {
        return Status(ErrorCodes::InvalidNamespace, str::stream() << "Invalid namespace: " << ns);
    }

    if (!NamespaceString::validCollectionName(ns.coll())) {
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "Invalid collection name: " << ns.coll());
    }

    if (serverGlobalParams.clusterRole == ClusterRole::ConfigServer && !ns.isOnInternalDb()) {
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream()
                          << "Can't create user databases on a --configsvr instance " << ns);
    }

    if (ns.isSystemDotProfile()) {
        return Status::OK();
    }

    if (ns.isSystem() && !ns.isLegalClientSystemNS(serverGlobalParams.featureCompatibility)) {
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "Invalid system namespace: " << ns);
    }

    if (ns.isNormalCollection() && ns.size() > NamespaceString::MaxNsCollectionLen) {
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "Fully qualified namespace is too long. Namespace: " << ns
                                    << " Max: " << NamespaceString::MaxNsCollectionLen);
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

        if (ns.isConfigDB() && ns.isLegalClientSystemNS(serverGlobalParams.featureCompatibility)) {
            return Status::OK();
        }

        return Status(ErrorCodes::BadValue, str::stream() << "Invalid namespace: " << ns);
    }

    return Status::OK();
}
}  // namespace mongo
