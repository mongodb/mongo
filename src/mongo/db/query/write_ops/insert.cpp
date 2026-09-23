// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/write_ops/insert.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/mongod_options_storage_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/util/validate_id.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/document_validation.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <string>
#include <vector>


namespace mongo {
StatusWith<BSONObj> fixDocumentForInsert(OperationContext* opCtx,
                                         const BSONObj& doc,
                                         bool bypassEmptyTsReplacement,
                                         bool* containsDotsAndDollarsField) {
    bool validationDisabled = DocumentValidationSettings::get(opCtx).isInternalValidationDisabled();

    if (validationDisabled) {
        if (containsDotsAndDollarsField) {
            BSONObjIterator i(doc);
            while (i.more()) {
                BSONElement e = i.next();
                if (e.fieldNameStringData().starts_with('$')) {
                    *containsDotsAndDollarsField = true;
                    // If the internal validation is disabled and we confirm this doc contains
                    // dots/dollars field name, we can skip other validations below.
                    break;
                }
            }
        }
        return StatusWith<BSONObj>(BSONObj());
    }

    // 'gAllowDocumentsGreaterThanMaxUserSize' should only ever be enabled when restoring a node
    // from a backup. For some restores, we re-insert whole oplog entries from a source cluster to a
    // destination cluster. Some generated oplog entries may exceed the user maximum due to entry
    // metadata, and therefore we should skip BSON size validation for these inserts. Note that we
    // should only skip the size check when inserting oplog entries into the oplog and not when
    // inserting user documents. The oplog entries to insert have already been validated for size on
    // the source cluster, and were successfully inserted into the source oplog.
    if (doc.objsize() > BSONObjMaxUserSize && !gAllowDocumentsGreaterThanMaxUserSize)
        return StatusWith<BSONObj>(ErrorCodes::BSONObjectTooLarge,
                                   str::stream() << "object to insert too large"
                                                 << ". size in bytes: " << doc.objsize()
                                                 << ", max size: " << BSONObjMaxUserSize);

    struct InsertDocAnalysis {
        bool firstElementIsId = false;
        bool hasTimestampToFix = false;
        boost::optional<BSONElement> idElement;
    };

    struct InsertDocVisitor {
        void checkDollarField(const BSONElement& e) {
            if (containsDotsAndDollarsField && e.fieldNameStringData().starts_with('$')) {
                *containsDotsAndDollarsField = true;
            }
        }

        void checkTimestampToFix(const BSONElement& e) {
            if (!bypassEmptyTsReplacement && e.type() == BSONType::timestamp &&
                e.timestampValue() == 0) {
                // We replace Timestamp(0,0) at the top level with a correct
                // value in the fast pass; we just mark that we want to swap.
                analysis.hasTimestampToFix = true;
            }
        }

        void checkIdField(const BSONElement& e) {
            if (e.fieldNameStringData() != "_id"sv) {
                return;
            }

            // check no regexp for _id (SERVER-9502)
            // also, disallow undefined and arrays
            // Make sure _id isn't duplicated (SERVER-19361).
            uassertStatusOK(validIdField(e));
            uassert(ErrorCodes::BadValue,
                    "can't have multiple _id fields in one document",
                    !analysis.idElement);
            analysis.firstElementIsId = isFirstElement;
            analysis.idElement = e;
        }

        void operator()(const BSONElement& e) {
            checkDollarField(e);
            checkTimestampToFix(e);
            checkIdField(e);
            isFirstElement = false;
        }

        InsertDocAnalysis& analysis;
        bool bypassEmptyTsReplacement;
        bool* containsDotsAndDollarsField;
        bool isFirstElement = true;
    };

    InsertDocAnalysis analysis;
    InsertDocVisitor visitor{analysis, bypassEmptyTsReplacement, containsDotsAndDollarsField};

    if (auto status = validateBSONDepthForUserStorage(doc, visitor); !status.isOK()) {
        return status.addContext("cannot insert document");
    }

    if (analysis.firstElementIsId && !analysis.hasTimestampToFix)
        return StatusWith<BSONObj>(BSONObj());

    BSONObjIterator i(doc);

    BSONObjBuilder b(doc.objsize() + 16);
    if (analysis.firstElementIsId) {
        b.append(doc.firstElement());
        i.next();
    } else if (analysis.idElement) {
        b.append(*analysis.idElement);
    } else {
        b.appendOID("_id"sv, nullptr, true);
    }

    while (i.more()) {
        BSONElement e = i.next();
        if (analysis.idElement && e.rawdata() == analysis.idElement->rawdata()) {
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
        if (rss::ReplicatedStorageService::get(opCtx)
                .getPersistenceProvider()
                .supportsLocalCollections()) {
            return Status::OK();
        }
        return Status(ErrorCodes::InvalidNamespace,
                      "system.profile unsupported when local collections are not supported");
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

    if (ns.isLocalDB() &&
        !rss::ReplicatedStorageService::get(opCtx)
             .getPersistenceProvider()
             .supportsLocalCollections()) {
        return Status(ErrorCodes::InvalidNamespace, "Local collections are not supported");
    }

    return Status::OK();
}
}  // namespace mongo
