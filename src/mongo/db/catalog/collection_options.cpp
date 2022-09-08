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

#include "mongo/db/catalog/collection_options.h"

#include <algorithm>

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection_options_validation.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/create_gen.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/idl/command_generic_argument.h"
#include "mongo/util/overloaded_visitor.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {
long long adjustCappedSize(long long cappedSize) {
    if (serverGlobalParams.featureCompatibility.isVersionInitialized() &&
        !feature_flags::gfeatureFlagCappedCollectionsRelaxedSize.isEnabled(
            serverGlobalParams.featureCompatibility)) {
        cappedSize += 0xff;
        cappedSize &= 0xffffffffffffff00LL;
    }
    return cappedSize;
}

long long adjustCappedMaxDocs(long long cappedMaxDocs) {
    if (cappedMaxDocs <= 0 || cappedMaxDocs == std::numeric_limits<long long>::max()) {
        cappedMaxDocs = 0x7fffffff;
    }
    return cappedMaxDocs;
}

void setEncryptedDefaultEncryptedCollectionNames(const NamespaceString& ns,
                                                 EncryptedFieldConfig* config) {
    auto prefix = std::string("enxcol_.") + ns.coll();

    if (!config->getEscCollection()) {
        config->setEscCollection(StringData(prefix + ".esc"));
    }

    if (!config->getEccCollection()) {
        config->setEccCollection(StringData(prefix + ".ecc"));
    }

    if (!config->getEcocCollection()) {
        config->setEcocCollection(StringData(prefix + ".ecoc"));
    }
}

}  // namespace

StatusWith<long long> CollectionOptions::checkAndAdjustCappedSize(long long cappedSize) {
    const long long kGB = 1024 * 1024 * 1024;
    const long long kPB = 1024 * 1024 * kGB;

    if (cappedSize < 0) {
        return Status(ErrorCodes::BadValue, "size has to be >= 0");
    }
    if (cappedSize > kPB) {
        return Status(ErrorCodes::BadValue, "size cannot exceed 1 PB");
    }

    return adjustCappedSize(cappedSize);
}

StatusWith<long long> CollectionOptions::checkAndAdjustCappedMaxDocs(long long cappedMaxDocs) {
    if (cappedMaxDocs >= 0x1LL << 31) {
        return Status(ErrorCodes::BadValue,
                      "max in a capped collection has to be < 2^31 or not set");
    }

    return adjustCappedMaxDocs(cappedMaxDocs);
}

bool CollectionOptions::isView() const {
    return !viewOn.empty();
}

Status CollectionOptions::validateForStorage() const {
    return CollectionOptions::parse(toBSON(), ParseKind::parseForStorage).getStatus();
}

StatusWith<CollectionOptions> CollectionOptions::parse(const BSONObj& options, ParseKind kind) {
    CollectionOptions collectionOptions;
    // Versions 2.4 and earlier of the server store "create" inside the collection metadata when the
    // user issues an explicit collection creation command. These versions also wrote any
    // unrecognized fields into the catalog metadata and allowed the order of these fields to be
    // changed. Therefore, if the "create" field is present, we must ignore any
    // unknown fields during parsing. Otherwise, we disallow unknown collection options.
    //
    // Versions 2.6 through 3.2 ignored unknown collection options rather than failing but did not
    // store the "create" field. These versions also refrained from materializing the unknown
    // options in the catalog, so we are free to fail on unknown options in this case.
    const bool createdOn24OrEarlier = static_cast<bool>(options["create"]);

    // During parsing, ignore some validation errors in order to accept options objects that
    // were valid in previous versions of the server.  SERVER-13737.
    BSONObjIterator i(options);

    while (i.more()) {
        BSONElement e = i.next();
        StringData fieldName = e.fieldName();

        if (fieldName == "uuid" && kind == parseForStorage) {
            auto res = UUID::parse(e);
            if (!res.isOK()) {
                return res.getStatus();
            }
            collectionOptions.uuid = res.getValue();
        } else if (fieldName == "capped") {
            collectionOptions.capped = e.trueValue();
        } else if (fieldName == "size") {
            if (!e.isNumber()) {
                // Ignoring for backwards compatibility.
                continue;
            }
            auto swCappedSize = checkAndAdjustCappedSize(e.safeNumberLong());
            if (!swCappedSize.isOK()) {
                return swCappedSize.getStatus();
            }
            collectionOptions.cappedSize = swCappedSize.getValue();
        } else if (fieldName == "max") {
            if (!options["capped"].trueValue() || !e.isNumber()) {
                // Ignoring for backwards compatibility.
                continue;
            }
            auto swCappedMaxDocs = checkAndAdjustCappedMaxDocs(e.safeNumberLong());
            if (!swCappedMaxDocs.isOK()) {
                return swCappedMaxDocs.getStatus();
            }
            collectionOptions.cappedMaxDocs = swCappedMaxDocs.getValue();
        } else if (fieldName == "$nExtents") {
            // Ignoring for backwards compatibility.
            continue;
        } else if (fieldName == "autoIndexId") {
            if (e.trueValue())
                collectionOptions.autoIndexId = YES;
            else
                collectionOptions.autoIndexId = NO;
        } else if (fieldName == "flags") {
            // Ignoring this field as it is deprecated.
            continue;
        } else if (fieldName == "temp") {
            collectionOptions.temp = e.trueValue();
        } else if (fieldName == "recordPreImages") {
            collectionOptions.recordPreImages = e.trueValue();
        } else if (fieldName == "changeStreamPreAndPostImages") {
            if (e.type() != mongo::Object) {
                return {ErrorCodes::InvalidOptions,
                        "'changeStreamPreAndPostImages' option must be a document"};
            }

            try {
                collectionOptions.changeStreamPreAndPostImagesOptions =
                    ChangeStreamPreAndPostImagesOptions::parse(
                        IDLParserContext{"changeStreamPreAndPostImagesOptions"}, e.Obj());
            } catch (const DBException& ex) {
                return ex.toStatus();
            }
        } else if (fieldName == "storageEngine") {
            if (e.type() != mongo::Object) {
                return {ErrorCodes::TypeMismatch, "'storageEngine' must be a document"};
            }

            auto status = collection_options_validation::validateStorageEngineOptions(e.Obj());
            if (!status.isOK()) {
                return status;
            }

            collectionOptions.storageEngine = e.Obj().getOwned();
        } else if (fieldName == "indexOptionDefaults") {
            if (e.type() != mongo::Object) {
                return {ErrorCodes::TypeMismatch, "'indexOptionDefaults' has to be a document."};
            }

            try {
                collectionOptions.indexOptionDefaults = IndexOptionDefaults::parse(
                    IDLParserContext{"CollectionOptions::parse"}, e.Obj());
            } catch (const DBException& ex) {
                return ex.toStatus();
            }
        } else if (fieldName == "validator") {
            if (e.type() != mongo::Object) {
                return Status(ErrorCodes::BadValue, "'validator' has to be a document.");
            }

            collectionOptions.validator = e.Obj().getOwned();
        } else if (fieldName == "validationAction") {
            if (e.type() != mongo::String) {
                return Status(ErrorCodes::BadValue, "'validationAction' has to be a string.");
            }

            try {
                collectionOptions.validationAction =
                    ValidationAction_parse(IDLParserContext{"validationAction"}, e.String());
            } catch (const DBException& exc) {
                return exc.toStatus();
            }
        } else if (fieldName == "validationLevel") {
            if (e.type() != mongo::String) {
                return Status(ErrorCodes::BadValue, "'validationLevel' has to be a string.");
            }

            try {
                collectionOptions.validationLevel =
                    ValidationLevel_parse(IDLParserContext{"validationLevel"}, e.String());
            } catch (const DBException& exc) {
                return exc.toStatus();
            }
        } else if (fieldName == "collation") {
            if (e.type() != mongo::Object) {
                return Status(ErrorCodes::BadValue, "'collation' has to be a document.");
            }

            if (e.Obj().isEmpty()) {
                return Status(ErrorCodes::BadValue, "'collation' cannot be an empty document.");
            }

            collectionOptions.collation = e.Obj().getOwned();
        } else if (fieldName == "clusteredIndex") {
            try {
                collectionOptions.clusteredIndex = clustered_util::parseClusteredInfo(e);
            } catch (const DBException& ex) {
                return ex.toStatus();
            }
        } else if (fieldName == "expireAfterSeconds") {
            if (e.type() != mongo::NumberLong) {
                return {ErrorCodes::BadValue, "'expireAfterSeconds' must be a number."};
            }

            collectionOptions.expireAfterSeconds = e.Long();
        } else if (fieldName == "viewOn") {
            if (e.type() != mongo::String) {
                return Status(ErrorCodes::BadValue, "'viewOn' has to be a string.");
            }

            collectionOptions.viewOn = e.String();
            if (collectionOptions.viewOn.empty()) {
                return Status(ErrorCodes::BadValue, "'viewOn' cannot be empty.'");
            }
        } else if (fieldName == "pipeline") {
            if (e.type() != mongo::Array) {
                return Status(ErrorCodes::BadValue, "'pipeline' has to be an array.");
            }

            collectionOptions.pipeline = e.Obj().getOwned();
        } else if (fieldName == "idIndex" && kind == parseForCommand) {
            if (e.type() != mongo::Object) {
                return Status(ErrorCodes::TypeMismatch, "'idIndex' has to be an object.");
            }

            auto tempIdIndex = e.Obj().getOwned();
            if (tempIdIndex.isEmpty()) {
                return {ErrorCodes::FailedToParse, "idIndex cannot be empty"};
            }

            collectionOptions.idIndex = std::move(tempIdIndex);
        } else if (fieldName == "timeseries") {
            if (e.type() != mongo::Object) {
                return {ErrorCodes::TypeMismatch, "'timeseries' must be a document"};
            }

            try {
                collectionOptions.timeseries =
                    TimeseriesOptions::parse(IDLParserContext{"CollectionOptions::parse"}, e.Obj());
            } catch (const DBException& ex) {
                return ex.toStatus();
            }
        } else if (fieldName == "encryptedFields") {
            if (e.type() != mongo::Object) {
                return {ErrorCodes::TypeMismatch, "'encryptedFields' must be a document"};
            }

            try {
                collectionOptions.encryptedFieldConfig =
                    collection_options_validation::processAndValidateEncryptedFields(
                        EncryptedFieldConfig::parse(IDLParserContext{"CollectionOptions::parse"},
                                                    e.Obj()));
            } catch (const DBException& ex) {
                return ex.toStatus();
            }
        } else if (!createdOn24OrEarlier && !mongo::isGenericArgument(fieldName)) {
            return Status(ErrorCodes::InvalidOptions,
                          str::stream()
                              << "The field '" << fieldName
                              << "' is not a valid collection option. Options: " << options);
        }
    }

    if (collectionOptions.viewOn.empty() && !collectionOptions.pipeline.isEmpty()) {
        return Status(ErrorCodes::BadValue, "'pipeline' cannot be specified without 'viewOn'");
    }

    return collectionOptions;
}

CollectionOptions CollectionOptions::fromCreateCommand(const CreateCommand& cmd) {
    CollectionOptions options;

    options.validationLevel = cmd.getValidationLevel();
    options.validationAction = cmd.getValidationAction();
    options.capped = cmd.getCapped();
    if (auto size = cmd.getSize()) {
        options.cappedSize = adjustCappedSize(*size);
    }
    if (auto max = cmd.getMax()) {
        options.cappedMaxDocs = adjustCappedMaxDocs(*max);
    }
    if (auto autoIndexId = cmd.getAutoIndexId()) {
        options.autoIndexId = *autoIndexId ? YES : NO;
    }
    if (auto idIndex = cmd.getIdIndex()) {
        options.idIndex = std::move(*idIndex);
    }
    if (auto storageEngine = cmd.getStorageEngine()) {
        options.storageEngine = std::move(*storageEngine);
    }
    if (auto validator = cmd.getValidator()) {
        options.validator = std::move(*validator);
    }
    if (auto indexOptionDefaults = cmd.getIndexOptionDefaults()) {
        options.indexOptionDefaults = std::move(*indexOptionDefaults);
    }
    if (auto viewOn = cmd.getViewOn()) {
        options.viewOn = viewOn->toString();
    }
    if (auto pipeline = cmd.getPipeline()) {
        BSONArrayBuilder builder;
        for (const auto& item : *pipeline) {
            builder.append(std::move(item));
        }
        options.pipeline = std::move(builder.arr());
    }
    if (auto collation = cmd.getCollation()) {
        options.collation = collation->toBSON();
    }
    if (auto recordPreImages = cmd.getRecordPreImages()) {
        options.recordPreImages = *recordPreImages;
    }
    if (auto changeStreamPreAndPostImagesOptions = cmd.getChangeStreamPreAndPostImages()) {
        options.changeStreamPreAndPostImagesOptions = *changeStreamPreAndPostImagesOptions;
    }
    if (auto timeseries = cmd.getTimeseries()) {
        options.timeseries = std::move(*timeseries);
    }
    if (auto clusteredIndex = cmd.getClusteredIndex()) {
        stdx::visit(
            OverloadedVisitor{
                [&](bool isClustered) {
                    if (isClustered) {
                        options.clusteredIndex =
                            clustered_util::makeCanonicalClusteredInfoForLegacyFormat();
                    } else {
                        options.clusteredIndex = boost::none;
                    }
                },
                [&](const ClusteredIndexSpec& clusteredIndexSpec) {
                    options.clusteredIndex =
                        clustered_util::makeCanonicalClusteredInfo(clusteredIndexSpec);
                }},
            *clusteredIndex);
    }
    if (auto expireAfterSeconds = cmd.getExpireAfterSeconds()) {
        options.expireAfterSeconds = expireAfterSeconds;
    }
    if (auto temp = cmd.getTemp()) {
        options.temp = *temp;
    }
    if (auto encryptedFieldConfig = cmd.getEncryptedFields()) {
        options.encryptedFieldConfig = std::move(*encryptedFieldConfig);
        setEncryptedDefaultEncryptedCollectionNames(cmd.getNamespace(),
                                                    options.encryptedFieldConfig.get_ptr());
    }

    return options;
}

BSONObj CollectionOptions::toBSON(bool includeUUID, const StringDataSet& includeFields) const {
    BSONObjBuilder b;
    appendBSON(&b, includeUUID, includeFields);
    return b.obj();
}

void CollectionOptions::appendBSON(BSONObjBuilder* builder,
                                   bool includeUUID,
                                   const StringDataSet& includeFields) const {
    if (uuid && includeUUID) {
        builder->appendElements(uuid->toBSON());
    }

    auto shouldAppend = [&](StringData option) {
        return includeFields.empty() || includeFields.contains(option);
    };

    if (capped && shouldAppend(CreateCommand::kCappedFieldName)) {
        builder->appendBool(CreateCommand::kCappedFieldName, true);
        builder->appendNumber(CreateCommand::kSizeFieldName, cappedSize);

        if (cappedMaxDocs)
            builder->appendNumber(CreateCommand::kMaxFieldName, cappedMaxDocs);
    }

    if (autoIndexId != DEFAULT && shouldAppend(CreateCommand::kAutoIndexIdFieldName))
        builder->appendBool(CreateCommand::kAutoIndexIdFieldName, autoIndexId == YES);

    if (temp && shouldAppend(CreateCommand::kTempFieldName))
        builder->appendBool(CreateCommand::kTempFieldName, true);

    if (recordPreImages && shouldAppend(CreateCommand::kRecordPreImagesFieldName)) {
        builder->appendBool(CreateCommand::kRecordPreImagesFieldName, true);
    }

    if (changeStreamPreAndPostImagesOptions.getEnabled() &&
        shouldAppend(CreateCommand::kChangeStreamPreAndPostImagesFieldName)) {
        builder->append(CreateCommand::kChangeStreamPreAndPostImagesFieldName,
                        changeStreamPreAndPostImagesOptions.toBSON());
    }

    if (!storageEngine.isEmpty() && shouldAppend(CreateCommand::kStorageEngineFieldName)) {
        builder->append(CreateCommand::kStorageEngineFieldName, storageEngine);
    }

    if (indexOptionDefaults.getStorageEngine() &&
        shouldAppend(CreateCommand::kIndexOptionDefaultsFieldName)) {
        builder->append(CreateCommand::kIndexOptionDefaultsFieldName, indexOptionDefaults.toBSON());
    }

    if (!validator.isEmpty() && shouldAppend(CreateCommand::kValidatorFieldName)) {
        builder->append(CreateCommand::kValidatorFieldName, validator);
    }

    if (validationLevel && shouldAppend(CreateCommand::kValidationLevelFieldName)) {
        builder->append(CreateCommand::kValidationLevelFieldName,
                        ValidationLevel_serializer(*validationLevel));
    }

    if (validationAction && shouldAppend(CreateCommand::kValidationActionFieldName)) {
        builder->append(CreateCommand::kValidationActionFieldName,
                        ValidationAction_serializer(*validationAction));
    }

    if (!collation.isEmpty() && shouldAppend(CreateCommand::kCollationFieldName)) {
        builder->append(CreateCommand::kCollationFieldName, collation);
    }

    if (clusteredIndex && shouldAppend(CreateCommand::kClusteredIndexFieldName)) {
        if (clusteredIndex->getLegacyFormat()) {
            builder->append(CreateCommand::kClusteredIndexFieldName, true);
        } else {
            // Only append the user defined collection options.
            builder->append(CreateCommand::kClusteredIndexFieldName,
                            clusteredIndex->getIndexSpec().toBSON());
        }
    }

    if (expireAfterSeconds && shouldAppend(CreateCommand::kExpireAfterSecondsFieldName)) {
        builder->append(CreateCommand::kExpireAfterSecondsFieldName, *expireAfterSeconds);
    }

    if (!viewOn.empty() && shouldAppend(CreateCommand::kViewOnFieldName)) {
        builder->append(CreateCommand::kViewOnFieldName, viewOn);
    }

    if (!pipeline.isEmpty() && shouldAppend(CreateCommand::kPipelineFieldName)) {
        builder->appendArray(CreateCommand::kPipelineFieldName, pipeline);
    }

    if (!idIndex.isEmpty() && shouldAppend(CreateCommand::kIdIndexFieldName)) {
        builder->append(CreateCommand::kIdIndexFieldName, idIndex);
    }

    if (timeseries && shouldAppend(CreateCommand::kTimeseriesFieldName)) {
        builder->append(CreateCommand::kTimeseriesFieldName, timeseries->toBSON());
    }

    if (encryptedFieldConfig && shouldAppend(CreateCommand::kEncryptedFieldsFieldName)) {
        builder->append(CreateCommand::kEncryptedFieldsFieldName, encryptedFieldConfig->toBSON());
    }
}

bool CollectionOptions::matchesStorageOptions(const CollectionOptions& other,
                                              CollatorFactoryInterface* collatorFactory) const {
    if (capped != other.capped) {
        return false;
    }

    if (cappedSize != other.cappedSize) {
        return false;
    }

    if (cappedMaxDocs != other.cappedMaxDocs) {
        return false;
    }

    if (autoIndexId != other.autoIndexId) {
        return false;
    }

    if (recordPreImages != other.recordPreImages) {
        return false;
    }

    if (changeStreamPreAndPostImagesOptions != other.changeStreamPreAndPostImagesOptions) {
        return false;
    }

    if (temp != other.temp) {
        return false;
    }

    if (storageEngine.woCompare(other.storageEngine) != 0) {
        return false;
    }

    if (indexOptionDefaults.toBSON().woCompare(other.indexOptionDefaults.toBSON()) != 0) {
        return false;
    }

    if (validator.woCompare(other.validator) != 0) {
        return false;
    }

    if (validationAction != other.validationAction) {
        return false;
    }

    if (validationLevel != other.validationLevel) {
        return false;
    }

    // Note: the server can add more stuff on the collation options that were not specified in
    // the original user request. Use the collator to check for equivalence.
    auto myCollator =
        collation.isEmpty() ? nullptr : uassertStatusOK(collatorFactory->makeFromBSON(collation));
    auto otherCollator = other.collation.isEmpty()
        ? nullptr
        : uassertStatusOK(collatorFactory->makeFromBSON(other.collation));

    if (!CollatorInterface::collatorsMatch(myCollator.get(), otherCollator.get())) {
        return false;
    }

    if (viewOn != other.viewOn) {
        return false;
    }

    if (pipeline.woCompare(other.pipeline) != 0) {
        return false;
    }

    if ((timeseries && other.timeseries &&
         timeseries->toBSON().woCompare(other.timeseries->toBSON()) != 0) ||
        (timeseries == boost::none) != (other.timeseries == boost::none)) {
        return false;
    }

    if ((clusteredIndex && other.clusteredIndex &&
         clusteredIndex->toBSON().woCompare(other.clusteredIndex->toBSON()) != 0) ||
        (clusteredIndex == boost::none) != (other.clusteredIndex == boost::none)) {
        return false;
    }

    if ((encryptedFieldConfig && other.encryptedFieldConfig &&
         encryptedFieldConfig->toBSON().woCompare(other.encryptedFieldConfig->toBSON()) != 0) ||
        (encryptedFieldConfig == boost::none) != (other.encryptedFieldConfig == boost::none)) {
        return false;
    }

    if (expireAfterSeconds != other.expireAfterSeconds) {
        return false;
    }

    return true;
}
}  // namespace mongo
