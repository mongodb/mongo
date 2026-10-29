// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/type_tags.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/util/assert_util.h"
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
