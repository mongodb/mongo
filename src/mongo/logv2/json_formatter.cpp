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

#include "mongo/logv2/json_formatter.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/logv2/attributes.h"
#include "mongo/logv2/constants.h"
#include "mongo/logv2/log_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"
#include "mongo/util/str_escape.h"

#include <cstddef>
#include <functional>
#include <iterator>
#include <variant>

#include <boost/cstdint.hpp>
#include <boost/exception/exception.hpp>
#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/log/utility/formatting_ostream.hpp>
#include <boost/log/utility/formatting_ostream_fwd.hpp>
#include <boost/log/utility/value_ref.hpp>
#include <fmt/compile.h>
#include <fmt/format.h>

namespace mongo::logv2 {
namespace {

struct JSONValueExtractor {
    JSONValueExtractor(fmt::memory_buffer& buffer, size_t attributeMaxSize)
        : _buffer(buffer), _attributeMaxSize(attributeMaxSize) {}

    void operator()(const char* name, CustomAttributeValue const& val) {
        try {
            // Try to format as BSON first if available. Prefer BSONAppend if available as we might
            // only want the value and not the whole element.
            if (val.BSONAppend) {
                BSONObjBuilder builder;
                val.BSONAppend(builder, name);
                // This is a JSON subobject, no quotes needed
                storeUnquoted(name);
                // BSONObj must outlive BSONElement. See BSONElement, BSONObj::getField().
                auto obj = builder.done();
                BSONElement element = obj.getField(name);
                BSONObj truncated =
                    element.jsonStringBuffer(JsonStringFormat::ExtendedRelaxedV2_0_0,
                                             false,
                                             false,
                                             0,
                                             _buffer,
                                             bufferSizeToTriggerTruncation());
                addTruncationReport(name, truncated, element.size());
            } else if (val.BSONSerialize) {
                // This is a JSON subobject, no quotes needed
                BSONObjBuilder builder;
                val.BSONSerialize(builder);
                BSONObj obj = builder.done();
                storeUnquoted(name);
                BSONObj truncated = obj.jsonStringBuffer(JsonStringFormat::ExtendedRelaxedV2_0_0,
                                                         0,
                                                         false,
                                                         _buffer,
                                                         bufferSizeToTriggerTruncation());
                addTruncationReport(name, truncated, builder.done().objsize());

            } else if (val.toBSONArray) {
                // This is a JSON subarray, no quotes needed
                BSONArray arr = val.toBSONArray();
                storeUnquoted(name);
                BSONObj truncated = arr.jsonStringBuffer(JsonStringFormat::ExtendedRelaxedV2_0_0,
                                                         0,
                                                         true,
                                                         _buffer,
                                                         bufferSizeToTriggerTruncation());
                addTruncationReport(name, truncated, arr.objsize());

            } else if (val.stringSerialize) {
                fmt::memory_buffer intermediate;
                val.stringSerialize(intermediate);
                storeQuoted(name, StringData(intermediate.data(), intermediate.size()));
            } else {
                // This is a string, surround value with quotes
                storeQuoted(name, val.toString());
            }
        } catch (...) {
            Status s = exceptionToStatus();
            storeQuoted(name, std::string("Failed to serialize due to exception: ") + s.toString());
        }
    }

    void operator()(const char* name, const BSONObj& val) {
        // This is a JSON subobject, no quotes needed
        storeUnquoted(name);
        BSONObj truncated = val.jsonStringBuffer(JsonStringFormat::ExtendedRelaxedV2_0_0,
                                                 0,
                                                 false,
                                                 _buffer,
                                                 bufferSizeToTriggerTruncation());
        addTruncationReport(name, truncated, val.objsize());
    }

    void operator()(const char* name, const BSONArray& val) {
        // This is a JSON subobject, no quotes needed
        storeUnquoted(name);
        BSONObj truncated = val.jsonStringBuffer(JsonStringFormat::ExtendedRelaxedV2_0_0,
                                                 0,
                                                 true,
                                                 _buffer,
                                                 bufferSizeToTriggerTruncation());
        addTruncationReport(name, truncated, val.objsize());
    }

    void operator()(const char* name, StringData value) {
        storeQuoted(name, value);
    }

    template <typename Period>
    void operator()(const char* name, const Duration<Period>& value) {
        // A suffix is automatically prepended
        dassert(!StringData(name).ends_with(value.mongoUnitSuffix()));
        fmt::format_to(std::back_inserter(_buffer),
                       FMT_COMPILE(R"({}"{}{}":{})"),
                       _separator,
                       name,
                       value.mongoUnitSuffix(),
                       value.count());
        _separator = ","_sd;
    }

    template <typename T>
    void operator()(const char* name, const T& value) {
        storeUnquotedValue(name, value);
    }

    BSONObj truncated() {
        return _truncated.done();
    }

    BSONObj truncatedSizes() {
        return _truncatedSizes.done();
    }

private:
    void storeUnquoted(StringData name) {
        fmt::format_to(std::back_inserter(_buffer), FMT_COMPILE(R"({}"{}":)"), _separator, name);
        _separator = ","_sd;
    }

    template <typename T>
    void storeUnquotedValue(StringData name, const T& value) {
        fmt::format_to(
            std::back_inserter(_buffer), FMT_COMPILE(R"({}"{}":{})"), _separator, name, value);
        _separator = ","_sd;
    }

    template <typename T>
    void storeQuoted(StringData name, const T& value) {
        fmt::format_to(std::back_inserter(_buffer), FMT_COMPILE(R"({}"{}":")"), _separator, name);
        std::size_t before = _buffer.size();
        std::size_t wouldWrite = 0;
        std::size_t written = 0;
        str::escapeForJSON(
            _buffer, value, _attributeMaxSize ? _attributeMaxSize : std::string::npos, &wouldWrite);
        written = _buffer.size() - before;

        if (wouldWrite > written) {
            // The bounded escape may have reached the limit and
            // stopped writing while in the middle of a UTF-8 sequence,
            // in which case the incomplete UTF-8 octets at the tail of the
            // buffer have to be trimmed.
            // Push a dummy byte so that the UTF-8 safe truncation
            // will truncate back down to the correct size.
            _buffer.push_back('x');
            auto truncatedEnd =
                str::UTF8SafeTruncation(_buffer.begin() + before, _buffer.end(), written);

            BSONObjBuilder truncationInfo = _truncated.subobjStart(name);
            truncationInfo.append("type"_sd, typeName(BSONType::string));
            truncationInfo.append("size"_sd, static_cast<int64_t>(wouldWrite));
            truncationInfo.done();

            _buffer.resize(truncatedEnd - _buffer.begin());
        }

        _buffer.push_back('"');
        _separator = ","_sd;
    }

    std::size_t bufferSizeToTriggerTruncation() const {
        if (!_attributeMaxSize)
            return _attributeMaxSize;

        return _buffer.size() + _attributeMaxSize;
    }

    void addTruncationReport(StringData name, const BSONObj& truncated, int64_t objsize) {
        if (!truncated.isEmpty()) {
            _truncated.append(name, truncated);
            _truncatedSizes.append(name, objsize);
        }
    }

    fmt::memory_buffer& _buffer;
    BSONObjBuilder _truncated;
    BSONObjBuilder _truncatedSizes;
    StringData _separator = ""_sd;
    size_t _attributeMaxSize;
};
}  // namespace

void JSONFormatter::format(fmt::memory_buffer& buffer,
                           LogSeverity severity,
                           LogComponent component,
                           Date_t date,
                           int32_t id,
                           LogService service,
                           StringData context,
                           StringData message,
                           const TypeErasedAttributeStorage& attrs,
                           LogTag tags,
                           const std::string& tenant,
                           LogTruncation truncation) const {
    namespace c = constants;
    static constexpr auto kFmt = JsonStringFormat::ExtendedRelaxedV2_0_0;
    StringData severityString = severity.toStringDataCompact();
    StringData componentString = component.getNameForLog();
    StringData serviceString = getNameForLog(service);
    bool local = _timestampFormat == LogTimestampFormat::kISO8601Local;
    size_t attributeMaxSize = truncation != LogTruncation::Enabled
        ? 0
        : (_maxAttributeSizeKB != 0 ? _maxAttributeSizeKB->loadRelaxed() * 1024
                                    : c::kDefaultMaxAttributeOutputSizeKB * 1024);
    auto write = [&](StringData s) {
        buffer.append(s.data(), s.data() + s.size());
    };

    struct CommaTracker {
        StringData comma;
        size_t padDebt = 0;
    };

    // First call to `comma` emits nothing, subsequent calls emit ",", so this is a
    // prefix used to join elements. If there is any padding debt in the
    // tracker, then that amount of whitespace is emitted after the comma and the
    // debt is cleared.
    auto writeComma = [&](CommaTracker& tracker) {
        write(tracker.comma);
        tracker.comma = ","_sd;
        while (tracker.padDebt) {
            // Relies on substr's truncation.
            StringData space = "                "_sd.substr(0, tracker.padDebt);
            write(space);
            tracker.padDebt -= space.size();
        }
    };

    // Emit `f()` normally, noting its size. If it is narrower than `width`, we incur a
    // padding debt which is stored in the `tracker`. The next `comma` emitted by the
    // `tracker` will be followed by enough whitespace to pay off the debt.
    auto padNextComma = [&](CommaTracker& tracker, size_t width, auto f) {
        return [&, width, f] {
            size_t pre = buffer.size();
            f();
            if (size_t wrote = buffer.size() - pre; wrote < width)
                tracker.padDebt = width - wrote;
        };
    };

    auto strFn = [&](StringData s) {
        return [&, s] {
            write(s);
        };
    };
    auto escFn = [&](StringData s) {
        return [&, s] {
            str::escapeForJSON(buffer, s);
        };
    };
    auto intFn = [&](auto x) {
        return [&, x] {
            fmt::format_int s{x};
            write(StringData{s.data(), s.size()});
        };
    };

    auto quote = [&](auto f) {
        return [&, f] {
            write("\"");
            f();
            write("\"");
        };
    };

    auto field = [&](CommaTracker& tracker, StringData name, auto f) {
        writeComma(tracker);
        quote(strFn(name))();
        write(":");
        f();
    };

    auto jsobj = [&](auto f) {
        return [&, f] {
            CommaTracker top;
            write("{");
            f(top);
            write("}");
        };
    };

    auto dateFn = [&](Date_t dateToPrint) {
        return jsobj([&, dateToPrint](CommaTracker& tracker) {
            field(tracker, "$date", quote([&, dateToPrint] {
                      write(StringData{DateStringBuffer{}.iso8601(dateToPrint, local)});
                  }));
        });
    };
    auto bsonObjFn = [&](BSONObj o) {
        return [&, o] {
            o.jsonStringBuffer(kFmt, 0, false, buffer, 0);
        };
    };
    auto bsonArrFn = [&](BSONArray a) {
        return [&, a] {
            a.jsonStringBuffer(kFmt, 0, true, buffer, 0);
        };
    };

    jsobj([&](CommaTracker& top) {
        field(top, c::kTimestampFieldName, dateFn(date));
        field(top, c::kSeverityFieldName, padNextComma(top, 5, quote(strFn(severityString))));
        field(top, c::kComponentFieldName, padNextComma(top, 11, quote(strFn(componentString))));
        field(top, c::kIdFieldName, padNextComma(top, 8, intFn(id)));
        if (!tenant.empty()) {
            field(top, c::kTenantFieldName, quote(strFn(tenant)));
        }
        if (shouldEmitLogService())
            field(top, c::kServiceFieldName, padNextComma(top, 4, quote(strFn(serviceString))));
        field(top, c::kContextFieldName, quote(strFn(context)));
        field(top, c::kMessageFieldName, quote(escFn(message)));
        if (!attrs.empty()) {
            JSONValueExtractor extractor(buffer, attributeMaxSize);
            field(top, c::kAttributesFieldName, jsobj([&](CommaTracker&) {
                      attrs.apply(extractor);
                  }));
            if (BSONObj o = extractor.truncated(); !o.isEmpty())
                field(top, c::kTruncatedFieldName, bsonObjFn(o));
            if (BSONObj o = extractor.truncatedSizes(); !o.isEmpty())
                field(top, c::kTruncatedSizeFieldName, bsonObjFn(o));
        }
        if (tags != LogTag::kNone) {
            field(top, c::kTagsFieldName, bsonArrFn(tags.toBSONArray()));
        }
    })();
}

void JSONFormatter::operator()(boost::log::record_view const& rec,
                               boost::log::formatting_ostream& strm) const {
    using boost::log::extract;

    fmt::memory_buffer buffer;

    const auto& tenant = extract<std::string>(attributes::tenant(), rec);
    format(buffer,
           extract<LogSeverity>(attributes::severity(), rec).get(),
           extract<LogComponent>(attributes::component(), rec).get(),
           extract<Date_t>(attributes::timeStamp(), rec).get(),
           extract<int32_t>(attributes::id(), rec).get(),
           extract<LogService>(attributes::service(), rec).get(),
           extract<StringData>(attributes::threadName(), rec).get(),
           extract<StringData>(attributes::message(), rec).get(),
           extract<TypeErasedAttributeStorage>(attributes::attributes(), rec).get(),
           extract<LogTag>(attributes::tags(), rec).get(),
           !tenant.empty() ? tenant.get() : std::string(),
           extract<LogTruncation>(attributes::truncation(), rec).get());

    // Write final JSON object to output stream
    strm.write(buffer.data(), buffer.size());
    strm.put(boost::log::formatting_ostream::char_type('\n'));
}

}  // namespace mongo::logv2
