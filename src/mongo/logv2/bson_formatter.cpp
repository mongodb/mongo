/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/logv2/bson_formatter.h"

#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/utility/formatting_ostream.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/attributes.h"
#include "mongo/logv2/constants.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/logv2/name_extractor.h"
#include "mongo/util/time_support.h"

#include <fmt/format.h>

namespace mongo::logv2 {

namespace {
struct BSONValueExtractor {
    BSONValueExtractor(BSONObjBuilder& builder)
        : _builder(builder.subobjStart(constants::kAttributesFieldName)) {}

    ~BSONValueExtractor() {
        _builder.done();
    }

    void operator()(StringData name, CustomAttributeValue const& val) {
        // Try to format as BSON first if available. Prefer BSONAppend if available as we might only
        // want the value and not the whole element.
        if (val.BSONAppend) {
            val.BSONAppend(_builder, name);
        } else if (val.BSONSerialize) {
            BSONObjBuilder subObjBuilder = _builder.subobjStart(name);
            val.BSONSerialize(subObjBuilder);
            subObjBuilder.done();
        } else if (val.toBSONArray) {
            _builder.append(name, val.toBSONArray());
        } else if (val.stringSerialize) {
            fmt::memory_buffer buffer;
            val.stringSerialize(buffer);
            _builder.append(name, fmt::to_string(buffer));
        } else {
            _builder.append(name, val.toString());
        }
    }

    // BSONObj is coming as a pointer, the generic one handles references
    void operator()(StringData name, const BSONObj val) {
        _builder.append(name, val);
    }

    void operator()(StringData name, const BSONArray val) {
        _builder.append(name, val);
    }

    // BSON is lacking unsigned types, so store unsigned int32 as signed int64
    void operator()(StringData name, unsigned int val) {
        _builder.append(name, static_cast<long long>(val));
    }

    // BSON is lacking unsigned types, so store unsigned int64 as signed int64, users need to deal
    // with this.
    void operator()(StringData name, unsigned long long val) {
        _builder.append(name, static_cast<long long>(val));
    }

    template <typename Period>
    void operator()(StringData name, const Duration<Period>& value) {
        _builder.append(name.toString() + value.mongoUnitSuffix(), value.count());
    }

    template <typename T>
    void operator()(StringData name, const T& value) {
        _builder.append(name, value);
    }

private:
    BSONObjBuilder _builder;
};
}  // namespace

void BSONFormatter::operator()(boost::log::record_view const& rec, BSONObjBuilder& builder) const {
    using boost::log::extract;

    // Build a JSON object for the user attributes.
    const auto& attrs = extract<TypeErasedAttributeStorage>(attributes::attributes(), rec).get();

    builder.append(constants::kTimestampFieldName,
                   extract<Date_t>(attributes::timeStamp(), rec).get());
    builder.append(constants::kSeverityFieldName,
                   extract<LogSeverity>(attributes::severity(), rec).get().toStringDataCompact());
    builder.append(constants::kComponentFieldName,
                   extract<LogComponent>(attributes::component(), rec).get().getNameForLog());
    builder.append(constants::kIdFieldName, extract<int32_t>(attributes::id(), rec).get());
    builder.append(constants::kContextFieldName,
                   extract<StringData>(attributes::threadName(), rec).get());
    builder.append(constants::kMessageFieldName,
                   extract<StringData>(attributes::message(), rec).get());

    if (!attrs.empty()) {
        BSONValueExtractor extractor(builder);
        attrs.apply(extractor);
    }
    LogTag tags = extract<LogTag>(attributes::tags(), rec).get();
    if (tags != LogTag::kNone) {
        builder.append(constants::kTagsFieldName, tags.toBSONArray());
    }
}

void BSONFormatter::operator()(boost::log::record_view const& rec,
                               boost::log::formatting_ostream& strm) const {
    BSONObjBuilder builder;
    operator()(rec, builder);
    BSONObj obj = builder.done();
    strm.write(obj.objdata(), obj.objsize());
}

BSONObj BSONFormatter::operator()(boost::log::record_view const& rec) const {
    BSONObjBuilder builder;
    operator()(rec, builder);
    return builder.obj();
}

}  // namespace mongo::logv2
