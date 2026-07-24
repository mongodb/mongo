// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/index_builds/index_build_oplog_entry.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_builds/primary_driven/enabled.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/create_oplog_entry_gen.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/db/version_context.h"
#include "mongo/logv2/redaction.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
StatusWith<IndexBuildOplogEntry> IndexBuildOplogEntry::parse(OperationContext* opCtx,
                                                             const repl::OplogEntry& entry,
                                                             bool parseO2) {
    // Example 'o' field which takes the same form for all three oplog entries.
    // {
    //     < "startIndexBuild" | "commitIndexBuild" | "abortIndexBuild" > : "coll",
    //     "indexBuildUUID" : <UUID>,
    //     "indexes" : [
    //         {
    //             "key" : {
    //                 "x" : 1
    //             },
    //             "name" : "x_1",
    //             ...
    //         },
    //         {
    //             "key" : {
    //                 "k" : 1
    //             },
    //             "name" : "k_1",
    //             ...
    //         }
    //     ],
    //     "multikey" : [  // Only allowed for 'commitIndexBuild'.
    //         null,
    //         {
    //             "k": BinData(0,"AQ=="),
    //         },
    //     ],
    //     "cause" : <Object> // Only required for 'abortIndexBuild'.
    // }
    //
    //
    // Ensure the collection name is specified
    invariant(entry.getOpType() == repl::OpTypeEnum::kCommand);

    auto commandType = entry.getCommandType();
    invariant(commandType == repl::OplogEntry::CommandType::kStartIndexBuild ||
              commandType == repl::OplogEntry::CommandType::kCommitIndexBuild ||
              commandType == repl::OplogEntry::CommandType::kAbortIndexBuild);

    BSONObj obj = entry.getObject();
    BSONElement first = obj.firstElement();
    auto commandName = first.fieldNameStringData();
    if (first.type() != BSONType::string) {
        return {ErrorCodes::InvalidNamespace,
                str::stream() << commandName << " value must be a string"};
    }

    IndexBuildMethodEnum indexBuildMethod{IndexBuildMethodEnum::kHybrid};

    auto buildUUIDElem = obj.getField("indexBuildUUID");
    if (buildUUIDElem.eoo()) {
        return {ErrorCodes::BadValue, "Missing required field 'indexBuildUUID'"};
    }

    auto swBuildUUID = UUID::parse(buildUUIDElem);
    if (!swBuildUUID.isOK()) {
        return swBuildUUID.getStatus().withContext("Error parsing 'indexBuildUUID'");
    }

    auto indexesElem = obj.getField("indexes");
    if (indexesElem.eoo()) {
        return {ErrorCodes::BadValue, "Missing required field 'indexes'"};
    }

    if (indexesElem.type() != BSONType::array) {
        return {ErrorCodes::BadValue, "Field 'indexes' must be an array of index spec objects"};
    }

    std::vector<IndexBuildInfo> indexesVec;
    for (auto& indexElem : indexesElem.Array()) {
        if (!indexElem.isABSONObj()) {
            return {ErrorCodes::BadValue, "Element of 'indexes' must be an object"};
        }
        std::string indexName;
        auto status = bsonExtractStringField(indexElem.Obj(), "name", &indexName);
        if (!status.isOK()) {
            return status.withContext("Error extracting 'name' from index spec");
        }
        indexesVec.emplace_back(indexElem.Obj().getOwned(), boost::none);
    }

    auto multikeyElem = obj["multikey"];
    if (multikeyElem) {
        if (multikeyElem.type() != BSONType::array) {
            return {ErrorCodes::BadValue, "Field 'multikey' must be an array"};
        }
        if (commandType != repl::OplogEntry::CommandType::kCommitIndexBuild) {
            return {ErrorCodes::BadValue,
                    "Field 'multikey' can only be used with 'commitIndexBuild'"};
        }
    }

    std::vector<boost::optional<MultikeyPaths>> multikey;
    for (auto&& elem : multikeyElem ? multikeyElem.Obj() : BSONObj{}) {
        switch (elem.type()) {
            case BSONType::null:
                multikey.push_back(boost::none);
                break;
            case BSONType::object: {
                auto parsed = multikey_paths::parse(elem.Obj());
                if (!parsed.isOK()) {
                    return parsed.getStatus();
                }
                multikey.push_back(parsed.getValue());
                break;
            }
            default:
                return {ErrorCodes::BadValue,
                        "Multikey array can only contain null or object types"};
        }
    }

    if (!multikey.empty() && multikey.size() != indexesVec.size()) {
        return {ErrorCodes::BadValue,
                "Multikey array must have the same number of elements as indexes array"};
    }

    // Get the reason this index build was aborted on the primary.
    boost::optional<Status> cause;
    if (repl::OplogEntry::CommandType::kAbortIndexBuild == commandType) {
        auto causeElem = obj.getField("cause");
        if (causeElem.eoo()) {
            return {ErrorCodes::BadValue, "Missing required field 'cause'."};
        }
        if (causeElem.type() != BSONType::object) {
            return {ErrorCodes::BadValue, "Field 'cause' must be an object."};
        }
        auto causeStatusObj = causeElem.Obj();
        cause = getStatusFromCommandResult(causeStatusObj);
    }

    auto collUUID = entry.getUuid();
    invariant(collUUID, str::stream() << redact(entry.toBSONForLogging()));

    boost::optional<std::string> indexBuildIdent;
    if (auto o2 = entry.getObject2(); o2 && parseO2) {
        auto parsedO2 =
            repl::IndexBuildOplogEntryO2::parse(*o2, IDLParserContext("indexBuildOplogEntryO2"));
        auto indexes = parsedO2.getIndexes();

        if (indexesVec.size() != indexes.size()) {
            return {
                ErrorCodes::BadValue,
                fmt::format("'indexes' array sizes differ between o and o2 objects, got {} vs {}",
                            indexesVec.size(),
                            indexes.size())};
        }

        const auto validateInternalIdent = [](std::string_view ident) -> Status {
            if (mongo::ident::isValidIdent(ident)) {
                return Status::OK();
            }
            return {ErrorCodes::BadValue, fmt::format("Internal ident '{}' is not valid", ident)};
        };

        const bool o2HasInternalIdents =
            !indexes.empty() && static_cast<bool>(indexes.front().getInternalIdents());
        for (const auto& indexIdents : indexes) {
            if (static_cast<bool>(indexIdents.getInternalIdents()) != o2HasInternalIdents) {
                return {ErrorCodes::BadValue,
                        "All indexes in 'o2.indexes' must either include or omit "
                        "'internalIdents'"};
            }
        }

        const bool pdibEnabled = index_builds::primary_driven::enabled(opCtx);

        if (o2HasInternalIdents && !pdibEnabled) {
            return {ErrorCodes::BadValue,
                    "'internalIdents' may only appear when primary-driven index builds are "
                    "enabled"};
        }

        if (o2HasInternalIdents) {
            indexBuildMethod = IndexBuildMethodEnum::kPrimaryDriven;
        }

        if (const auto& parsedIdent = parsedO2.getIndexBuildIdent()) {
            if (!pdibEnabled) {
                return {ErrorCodes::BadValue,
                        "'indexBuildIdent' may only appear when primary-driven index builds are "
                        "enabled"};
            }
            if (auto status = validateInternalIdent(*parsedIdent); !status.isOK()) {
                return status;
            }
            indexBuildIdent = std::string{*parsedIdent};
        }

        auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
        for (size_t i = 0; i < indexes.size(); ++i) {
            if (o2HasInternalIdents) {
                const auto& internalIdents = *indexes[i].getInternalIdents();
                for (const auto ident : {internalIdents.getSorterIdent(),
                                         internalIdents.getSideWritesIdent(),
                                         internalIdents.getSkippedRecordsIdent()}) {
                    if (auto status = validateInternalIdent(ident); !status.isOK()) {
                        return status;
                    }
                }
                if (const auto& constraintViolationsIdent =
                        internalIdents.getConstraintViolationsIdent()) {
                    if (auto status = validateInternalIdent(*constraintViolationsIdent);
                        !status.isOK()) {
                        return status;
                    }
                } else if (indexesVec[i].spec["unique"].trueValue() ||
                           IndexDescriptor::isIdIndexPattern(
                               indexesVec[i].spec.getObjectField("key"))) {
                    return {ErrorCodes::BadValue,
                            "constraintViolationsIdent is required for unique and _id "
                            "indexes"};
                }
                indexesVec[i].setInternalIdents(
                    std::string{internalIdents.getSorterIdent()},
                    std::string{internalIdents.getSideWritesIdent()},
                    std::string{internalIdents.getSkippedRecordsIdent()},
                    internalIdents.getConstraintViolationsIdent()
                        ? boost::make_optional(
                              std::string{*internalIdents.getConstraintViolationsIdent()})
                        : boost::none);
            }

            auto indexIdentUniqueTag = indexes[i].getIndexIdent();
            if (!ident::validateTag(indexIdentUniqueTag)) {
                return {ErrorCodes::BadValue,
                        fmt::format("'indexIdent' '{}' is not valid", indexIdentUniqueTag)};
            }

            const auto& indexIdent =
                storageEngine->generateNewIndexIdent(entry.getNss().dbName(), indexIdentUniqueTag);
            indexesVec[i].indexIdent = indexIdent;
        }
    }

    return IndexBuildOplogEntry{*collUUID,
                                commandType,
                                std::string{commandName},
                                indexBuildMethod,
                                swBuildUUID.getValue(),
                                std::move(indexesVec),
                                std::move(multikey),
                                cause,
                                entry.getOpTime(),
                                std::move(indexBuildIdent)};
}
}  // namespace mongo
