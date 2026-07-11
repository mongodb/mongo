// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/logv2/bson_formatter.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/attribute_storage.h"
#include "mongo/logv2/attributes.h"
#include "mongo/logv2/constants.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_service.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/logv2/log_tag.h"
#include "mongo/logv2/log_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <functional>
#include <string>
#include <string_view>
#include <variant>

#include <boost/cstdint.hpp>
#include <boost/exception/exception.hpp>
#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/log/utility/formatting_ostream.hpp>
#include <boost/log/utility/formatting_ostream_fwd.hpp>
#include <boost/log/utility/value_ref.hpp>
#include <fmt/format.h>

namespace mongo::logv2 {

namespace {
struct BSONValueExtractor {
    BSONValueExtractor(BSONObjBuilder& builder)
        : _builder(builder.subobjStart(constants::kAttributesFieldName)) {}

    ~BSONValueExtractor() {
        _builder.done();
    }

    void operator()(std::string_view name, CustomAttributeValue const& val) {
        try {
            // Try to format as BSON first if available. Prefer BSONAppend if available as we might
            // only want the value and not the whole element.
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
        } catch (...) {
            Status s = exceptionToStatus();
            _builder.append(name,
                            std::string("Failed to serialize due to exception") + s.toString());
        }
    }

    // BSONObj is coming as a pointer, the generic one handles references
    void operator()(std::string_view name, const BSONObj val) {
        _builder.append(name, val);
    }

    void operator()(std::string_view name, const BSONArray val) {
        _builder.append(name, val);
    }

    // BSON is lacking unsigned types, so store unsigned int32 as signed int64
    void operator()(std::string_view name, unsigned int val) {
        _builder.append(name, static_cast<long long>(val));
    }

    // BSON is lacking unsigned types, so store unsigned int64 as signed int64, users need to deal
    // with this.
    void operator()(std::string_view name, unsigned long long val) {
        _builder.append(name, static_cast<long long>(val));
    }

    template <typename Period>
    void operator()(std::string_view name, const Duration<Period>& value) {
        _builder.append(fmt::format("{}{}", name, value.mongoUnitSuffix()), value.count());
    }

    template <typename T>
    void operator()(std::string_view name, const T& value) {
        _builder.append(name, value);
    }

private:
    BSONObjBuilder _builder;
};
}  // namespace

void BSONFormatter::operator()(boost::log::record_view const& rec, BSONObjBuilder& builder) const {
    using boost::log::extract;

    // Build a JSON object for the user attributes.
    const auto& attrs = extract<TypeErasedAttributeStorage>(attributes::attributes(), rec);

    builder.append(constants::kTimestampFieldName,
                   extract<Date_t>(attributes::timeStamp(), rec).get());
    builder.append(constants::kSeverityFieldName,
                   extract<LogSeverity>(attributes::severity(), rec).get().toStringDataCompact());
    builder.append(constants::kComponentFieldName,
                   extract<LogComponent>(attributes::component(), rec).get().getNameForLog());
    builder.append(constants::kIdFieldName, extract<int32_t>(attributes::id(), rec).get());
    const auto& tenant = extract<std::string>(attributes::tenant(), rec);
    if (!tenant.empty()) {
        builder.append(constants::kTenantFieldName, tenant.get());
    }
    if (shouldEmitLogService())
        builder.append(constants::kServiceFieldName,
                       getNameForLog(extract<LogService>(attributes::service(), rec).get()));
    builder.append(constants::kContextFieldName,
                   extract<std::string_view>(attributes::threadName(), rec).get());
    builder.append(constants::kMessageFieldName,
                   extract<std::string_view>(attributes::message(), rec).get());

    if (!attrs.get().empty()) {
        BSONValueExtractor extractor(builder);
        attrs.get().apply(extractor);
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
