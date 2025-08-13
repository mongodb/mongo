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

#include "mongo/db/global_catalog/type_tags.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"

#include <cstring>
#include <type_traits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

using std::string;

const NamespaceString TagsType::ConfigNS(NamespaceString::kConfigsvrTagsNamespace);

const BSONField<std::string> TagsType::ns("ns");
const BSONField<std::string> TagsType::tag("tag");
const BSONField<BSONObj> TagsType::min("min");
const BSONField<BSONObj> TagsType::max("max");

TagsType::TagsType(NamespaceString nss, std::string tag, ChunkRange range)
    : _ns(std::move(nss)), _tag(std::move(tag)), _range(std::move(range)) {}

StatusWith<TagsType> TagsType::fromBSON(const BSONObj& source) {
    TagsType tags;

    {
        std::string tagsNs;
        Status status = bsonExtractStringField(source, ns.name(), &tagsNs);
        if (!status.isOK()) {
            return status;
        }

        tags._ns = NamespaceStringUtil::deserialize(
            boost::none, tagsNs, SerializationContext::stateDefault());
    }

    {
        std::string tagsTag;
        Status status = bsonExtractStringField(source, tag.name(), &tagsTag);
        if (!status.isOK()) {
            return status;
        }

        tags._tag = std::move(tagsTag);
    }

    {
        try {
            tags._range = ChunkRange::fromBSON(source);
        } catch (const DBException& e) {
            return e.toStatus().withContext("Failed to parse chunk range");
        }
    }

    return tags;
}

Status TagsType::validate() const {
    if (!_ns.has_value() || !_ns->isValid()) {
        return Status(ErrorCodes::NoSuchKey, str::stream() << "missing " << ns.name() << " field");
    }

    if (!_tag.has_value() || _tag->empty()) {
        return Status(ErrorCodes::NoSuchKey, str::stream() << "missing " << tag.name() << " field");
    }

    if (!_range.has_value()) {
        return Status(ErrorCodes::NoSuchKey, str::stream() << "missing range field");
    }

    auto rangeValidationStatus = ChunkRange::validateStrict(*_range);
    if (!rangeValidationStatus.isOK()) {
        return rangeValidationStatus;
    }

    return Status::OK();
}

BSONObj TagsType::toBSON() const {
    BSONObjBuilder builder;

    if (_ns)
        builder.append(
            ns.name(),
            NamespaceStringUtil::serialize(getNS(), SerializationContext::stateDefault()));
    if (_tag)
        builder.append(tag.name(), getTag());
    if (_range) {
        _range->serialize(&builder);
    }

    return builder.obj();
}

std::string TagsType::toString() const {
    return toBSON().toString();
}

void TagsType::setNS(const NamespaceString& ns) {
    invariant(ns.isValid());
    _ns = ns;
}

void TagsType::setTag(const std::string& tag) {
    invariant(!tag.empty());
    _tag = tag;
}

void TagsType::setRange(const ChunkRange& range) {
    _range = range;
}

}  // namespace mongo
