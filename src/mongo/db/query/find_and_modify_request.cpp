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

#include "mongo/db/query/find_and_modify_request.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/command_generic_argument.h"
#include "mongo/db/write_concern.h"
#include "mongo/idl/idl_parser.h"

namespace mongo {

namespace {
const char kCmdName[] = "findAndModify";
const char kQueryField[] = "query";
const char kSortField[] = "sort";
const char kCollationField[] = "collation";
const char kArrayFiltersField[] = "arrayFilters";
const char kRemoveField[] = "remove";
const char kUpdateField[] = "update";
const char kNewField[] = "new";
const char kFieldProjectionField[] = "fields";
const char kUpsertField[] = "upsert";
const char kWriteConcernField[] = "writeConcern";

const std::vector<BSONObj> emptyArrayFilters{};

const std::vector<StringData> _knownFields{
    FindAndModifyRequest::kBypassDocumentValidationFieldName,
    FindAndModifyRequest::kLegacyCommandName,
    FindAndModifyRequest::kCommandName,
};
}  // unnamed namespace

FindAndModifyRequest::FindAndModifyRequest(NamespaceString fullNs,
                                           BSONObj query,
                                           boost::optional<write_ops::UpdateModification> update)
    : _ns(std::move(fullNs)), _query(query.getOwned()), _update(std::move(update)) {}

FindAndModifyRequest FindAndModifyRequest::makeUpdate(NamespaceString fullNs,
                                                      BSONObj query,
                                                      write_ops::UpdateModification update) {
    return FindAndModifyRequest(fullNs, query, std::move(update));
}

FindAndModifyRequest FindAndModifyRequest::makeRemove(NamespaceString fullNs, BSONObj query) {
    FindAndModifyRequest request(fullNs, query, {});
    return request;
}

BSONObj FindAndModifyRequest::toBSON(const BSONObj& commandPassthroughFields) const {
    BSONObjBuilder builder;

    builder.append(kCmdName, _ns.coll());
    builder.append(kQueryField, _query);

    if (_update) {
        _update->serializeToBSON(kUpdateField, &builder);

        if (_isUpsert) {
            builder.append(kUpsertField, _isUpsert.get());
        }
    } else {
        builder.append(kRemoveField, true);
    }

    if (_fieldProjection) {
        builder.append(kFieldProjectionField, _fieldProjection.get());
    }

    if (_sort) {
        builder.append(kSortField, _sort.get());
    }

    if (_collation) {
        builder.append(kCollationField, _collation.get());
    }

    if (_arrayFilters) {
        BSONArrayBuilder arrayBuilder(builder.subarrayStart(kArrayFiltersField));
        for (auto arrayFilter : _arrayFilters.get()) {
            arrayBuilder.append(arrayFilter);
        }
        arrayBuilder.doneFast();
    }

    if (_shouldReturnNew) {
        builder.append(kNewField, _shouldReturnNew.get());
    }

    if (_writeConcern) {
        builder.append(kWriteConcernField, _writeConcern->toBSON());
    }

    IDLParserErrorContext::appendGenericCommandArguments(
        commandPassthroughFields, _knownFields, &builder);

    return builder.obj();
}

StatusWith<FindAndModifyRequest> FindAndModifyRequest::parseFromBSON(NamespaceString fullNs,
                                                                     const BSONObj& cmdObj) {
    BSONObj query;
    BSONObj fields;
    BSONObj updateObj;
    BSONObj sort;
    boost::optional<write_ops::UpdateModification> update;

    BSONObj collation;
    bool shouldReturnNew = false;
    bool isUpsert = false;
    bool isRemove = false;
    bool arrayFiltersSet = false;
    std::vector<BSONObj> arrayFilters;

    for (auto&& field : cmdObj.getFieldNames<std::set<std::string>>()) {
        if (field == kQueryField) {
            query = cmdObj.getObjectField(kQueryField);
        } else if (field == kSortField) {
            sort = cmdObj.getObjectField(kSortField);
        } else if (field == kRemoveField) {
            isRemove = cmdObj[kRemoveField].trueValue();
        } else if (field == kUpdateField) {
            update = write_ops::UpdateModification::parseFromBSON(cmdObj[kUpdateField]);
        } else if (field == kNewField) {
            shouldReturnNew = cmdObj[kNewField].trueValue();
        } else if (field == kFieldProjectionField) {
            fields = cmdObj.getObjectField(kFieldProjectionField);
        } else if (field == kUpsertField) {
            isUpsert = cmdObj[kUpsertField].trueValue();
        } else if (field == kCollationField) {
            BSONElement collationElt;
            Status collationEltStatus =
                bsonExtractTypedField(cmdObj, kCollationField, BSONType::Object, &collationElt);
            if (!collationEltStatus.isOK() && (collationEltStatus != ErrorCodes::NoSuchKey)) {
                return collationEltStatus;
            }
            if (collationEltStatus.isOK()) {
                collation = collationElt.Obj();
            }
        } else if (field == kArrayFiltersField) {
            BSONElement arrayFiltersElt;
            Status arrayFiltersEltStatus = bsonExtractTypedField(
                cmdObj, kArrayFiltersField, BSONType::Array, &arrayFiltersElt);
            if (!arrayFiltersEltStatus.isOK() && (arrayFiltersEltStatus != ErrorCodes::NoSuchKey)) {
                return arrayFiltersEltStatus;
            }
            if (arrayFiltersEltStatus.isOK()) {
                arrayFiltersSet = true;
                for (auto arrayFilter : arrayFiltersElt.Obj()) {
                    if (arrayFilter.type() != BSONType::Object) {
                        return {ErrorCodes::TypeMismatch,
                                str::stream() << "Each array filter must be an object, found "
                                              << arrayFilter.type()};
                    }
                    arrayFilters.push_back(arrayFilter.Obj());
                }
            }
        } else if (!isGenericArgument(field) &&
                   !std::count(_knownFields.begin(), _knownFields.end(), field)) {
            return {ErrorCodes::Error(51177),
                    str::stream() << "BSON field '" << field << "' is an unknown field."};
        }
    }

    if (!isRemove && !update) {
        return {ErrorCodes::FailedToParse, "Either an update or remove=true must be specified"};
    }

    if (isRemove) {
        if (update) {
            return {ErrorCodes::FailedToParse, "Cannot specify both an update and remove=true"};
        }

        if (isUpsert) {
            return {ErrorCodes::FailedToParse, "Cannot specify both upsert=true and remove=true"};
        }

        if (shouldReturnNew) {
            return {ErrorCodes::FailedToParse,
                    "Cannot specify both new=true and remove=true;"
                    " 'remove' always returns the deleted document"};
        }

        if (arrayFiltersSet) {
            return {ErrorCodes::FailedToParse, "Cannot specify arrayFilters and remove=true"};
        }
    }

    if (update && update->type() == write_ops::UpdateModification::Type::kPipeline &&
        arrayFiltersSet) {
        return {ErrorCodes::FailedToParse, "Cannot specify arrayFilters and a pipeline update"};
    }

    FindAndModifyRequest request(std::move(fullNs), query, std::move(update));
    request.setFieldProjection(fields);
    request.setSort(sort);
    request.setCollation(collation);
    if (arrayFiltersSet) {
        request.setArrayFilters(std::move(arrayFilters));
    }

    if (!isRemove) {
        request.setShouldReturnNew(shouldReturnNew);
        request.setUpsert(isUpsert);
    }

    return request;
}

void FindAndModifyRequest::setFieldProjection(BSONObj fields) {
    _fieldProjection = fields.getOwned();
}

void FindAndModifyRequest::setSort(BSONObj sort) {
    _sort = sort.getOwned();
}

void FindAndModifyRequest::setCollation(BSONObj collation) {
    _collation = collation.getOwned();
}

void FindAndModifyRequest::setArrayFilters(const std::vector<BSONObj>& arrayFilters) {
    _arrayFilters = std::vector<BSONObj>();
    for (auto arrayFilter : arrayFilters) {
        _arrayFilters->emplace_back(arrayFilter.getOwned());
    }
}

void FindAndModifyRequest::setQuery(BSONObj query) {
    _query = query.getOwned();
}
void FindAndModifyRequest::setUpdateObj(BSONObj updateObj) {
    _update.emplace(updateObj.getOwned());
}

void FindAndModifyRequest::setShouldReturnNew(bool shouldReturnNew) {
    dassert(_update);
    _shouldReturnNew = shouldReturnNew;
}

void FindAndModifyRequest::setUpsert(bool upsert) {
    dassert(_update);
    _isUpsert = upsert;
}

void FindAndModifyRequest::setWriteConcern(WriteConcernOptions writeConcern) {
    _writeConcern = std::move(writeConcern);
}

const NamespaceString& FindAndModifyRequest::getNamespaceString() const {
    return _ns;
}

BSONObj FindAndModifyRequest::getQuery() const {
    return _query;
}

BSONObj FindAndModifyRequest::getFields() const {
    return _fieldProjection.value_or(BSONObj());
}

const boost::optional<write_ops::UpdateModification>& FindAndModifyRequest::getUpdate() const {
    return _update;
}

BSONObj FindAndModifyRequest::getSort() const {
    return _sort.value_or(BSONObj());
}

BSONObj FindAndModifyRequest::getCollation() const {
    return _collation.value_or(BSONObj());
}

const std::vector<BSONObj>& FindAndModifyRequest::getArrayFilters() const {
    if (_arrayFilters) {
        return _arrayFilters.get();
    }
    return emptyArrayFilters;
}

bool FindAndModifyRequest::shouldReturnNew() const {
    return _shouldReturnNew.value_or(false);
}

bool FindAndModifyRequest::isUpsert() const {
    return _isUpsert.value_or(false);
}

bool FindAndModifyRequest::isRemove() const {
    return !static_cast<bool>(_update);
}
}
