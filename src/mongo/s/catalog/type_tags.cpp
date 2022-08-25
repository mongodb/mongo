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

#include "mongo/s/catalog/type_tags.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

using std::string;

const NamespaceString TagsType::ConfigNS("config.tags");

const BSONField<std::string> TagsType::ns("ns");
const BSONField<std::string> TagsType::tag("tag");
const BSONField<BSONObj> TagsType::min("min");
const BSONField<BSONObj> TagsType::max("max");

TagsType::TagsType(NamespaceString nss, std::string tag, ChunkRange range)
    : _ns(std::move(nss)),
      _tag(std::move(tag)),
      _minKey(range.getMin().getOwned()),
      _maxKey(range.getMax().getOwned()) {}

StatusWith<TagsType> TagsType::fromBSON(const BSONObj& source) {
    TagsType tags;

    {
        std::string tagsNs;
        Status status = bsonExtractStringField(source, ns.name(), &tagsNs);
        if (!status.isOK()) {
            return status;
        }

        tags._ns = NamespaceString(tagsNs);
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
        auto tagRangeStatus = ChunkRange::fromBSON(source);
        if (!tagRangeStatus.isOK())
            return tagRangeStatus.getStatus();

        const auto tagRange = std::move(tagRangeStatus.getValue());
        tags._minKey = tagRange.getMin().getOwned();
        tags._maxKey = tagRange.getMax().getOwned();
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

    if (!_minKey.has_value() || _minKey->isEmpty()) {
        return Status(ErrorCodes::NoSuchKey, str::stream() << "missing " << min.name() << " field");
    }

    if (!_maxKey.has_value() || _maxKey->isEmpty()) {
        return Status(ErrorCodes::NoSuchKey, str::stream() << "missing " << max.name() << " field");
    }

    // 'min' and 'max' must share the same fields.
    if (_minKey->nFields() != _maxKey->nFields()) {
        return Status(ErrorCodes::BadValue, "min and max have a different number of keys");
    }

    BSONObjIterator minIt(_minKey.value());
    BSONObjIterator maxIt(_maxKey.value());
    while (minIt.more() && maxIt.more()) {
        BSONElement minElem = minIt.next();
        BSONElement maxElem = maxIt.next();
        if (strcmp(minElem.fieldName(), maxElem.fieldName()) != 0) {
            return Status(ErrorCodes::BadValue, "min and max have different set of keys");
        }
    }

    // 'max' should be greater than 'min'.
    if (_minKey->woCompare(_maxKey.value()) >= 0) {
        return Status(ErrorCodes::BadValue, "max key must be greater than min key");
    }

    return Status::OK();
}

BSONObj TagsType::toBSON() const {
    BSONObjBuilder builder;

    if (_ns)
        builder.append(ns.name(), getNS().ns());
    if (_tag)
        builder.append(tag.name(), getTag());
    if (_minKey)
        builder.append(min.name(), getMinKey());
    if (_maxKey)
        builder.append(max.name(), getMaxKey());

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

void TagsType::setMinKey(const BSONObj& minKey) {
    invariant(!minKey.isEmpty());
    _minKey = minKey;
}

void TagsType::setMaxKey(const BSONObj& maxKey) {
    invariant(!maxKey.isEmpty());
    _maxKey = maxKey;
}

}  // namespace mongo
