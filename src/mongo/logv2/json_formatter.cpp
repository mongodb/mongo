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

#include <boost/container/small_vector.hpp>
#include <boost/log/attributes/value_extraction.hpp>
#include <boost/log/expressions/message.hpp>
#include <boost/log/utility/formatting_ostream.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/attributes.h"
#include "mongo/logv2/constants.h"
#include "mongo/logv2/name_extractor.h"
#include "mongo/util/str_escape.h"

#include <fmt/compile.h>
#include <fmt/format.h>

namespace mongo::logv2 {
namespace {
template <typename CompiledFormatStr, typename... Args>
static void compiled_format_to(fmt::memory_buffer& buffer,
                               const CompiledFormatStr& fmt_str,
                               const Args&... args) {
    fmt::internal::cf::vformat_to<fmt::buffer_context<char>>(
        fmt::buffer_range(buffer),
        fmt_str,
        {fmt::make_format_args<fmt::buffer_context<char>>(args...)});
}

struct JSONValueExtractor {
    JSONValueExtractor(fmt::memory_buffer& buffer, size_t attributeMaxSize)
        : _buffer(buffer), _attributeMaxSize(attributeMaxSize) {}

    void operator()(StringData name, CustomAttributeValue const& val) {
        // Try to format as BSON first if available. Prefer BSONAppend if available as we might only
        // want the value and not the whole element.
        if (val.BSONAppend) {
            BSONObjBuilder builder;
            val.BSONAppend(builder, name);
            // This is a JSON subobject, no quotes needed
            storeUnquoted(name);
            BSONElement element = builder.done().getField(name);
            BSONObj truncated = element.jsonStringBuffer(JsonStringFormat::ExtendedRelaxedV2_0_0,
                                                         false,
                                                         false,
                                                         0,
                                                         _buffer,
                                                         bufferSizeToTriggerTruncation());
            addTruncationReport(name, truncated, element.size());
        } else if (val.BSONSerialize) {
            // This is a JSON subobject, no quotes needed
            storeUnquoted(name);
            BSONObjBuilder builder;
            val.BSONSerialize(builder);
            BSONObj obj = builder.done();
            BSONObj truncated = obj.jsonStringBuffer(JsonStringFormat::ExtendedRelaxedV2_0_0,
                                                     0,
                                                     false,
                                                     _buffer,
                                                     bufferSizeToTriggerTruncation());
            addTruncationReport(name, truncated, builder.done().objsize());

        } else if (val.toBSONArray) {
            // This is a JSON subarray, no quotes needed
            storeUnquoted(name);
            BSONArray arr = val.toBSONArray();
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
    }

    void operator()(StringData name, const BSONObj& val) {
        // This is a JSON subobject, no quotes needed
        storeUnquoted(name);
        BSONObj truncated = val.jsonStringBuffer(JsonStringFormat::ExtendedRelaxedV2_0_0,
                                                 0,
                                                 false,
                                                 _buffer,
                                                 bufferSizeToTriggerTruncation());
        addTruncationReport(name, truncated, val.objsize());
    }

    void operator()(StringData name, const BSONArray& val) {
        // This is a JSON subobject, no quotes needed
        storeUnquoted(name);
        BSONObj truncated = val.jsonStringBuffer(JsonStringFormat::ExtendedRelaxedV2_0_0,
                                                 0,
                                                 true,
                                                 _buffer,
                                                 bufferSizeToTriggerTruncation());
        addTruncationReport(name, truncated, val.objsize());
    }

    void operator()(StringData name, StringData value) {
        storeQuoted(name, value);
    }

    template <typename Period>
    void operator()(StringData name, const Duration<Period>& value) {
        // A suffix is automatically prepended
        dassert(!name.endsWith(value.mongoUnitSuffix()));
        static const auto& fmtStr =
            *new auto(fmt::compile<StringData, StringData, StringData, int64_t>(R"({}"{}{}":{})"));
        compiled_format_to(
            _buffer, fmtStr, _separator, name, value.mongoUnitSuffix(), value.count());
        _separator = ","_sd;
    }

    template <typename T>
    void operator()(StringData name, const T& value) {
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
        static const auto& fmtStr = *new auto(fmt::compile<StringData, StringData>(R"({}"{}":)"));
        compiled_format_to(_buffer, fmtStr, _separator, name);
        _separator = ","_sd;
    }

    template <typename T>
    void storeUnquotedValue(StringData name, const T& value) {
        static const auto& fmtStr =
            *new auto(fmt::compile<StringData, StringData, T>(R"({}"{}":{})"));
        compiled_format_to(_buffer, fmtStr, _separator, name, value);
        _separator = ","_sd;
    }

    template <typename T>
    void storeQuoted(StringData name, const T& value) {
        static const auto& fmtStr = *new auto(fmt::compile<StringData, StringData>(R"({}"{}":")"));
        compiled_format_to(_buffer, fmtStr, _separator, name);
        std::size_t before = _buffer.size();
        str::escapeForJSON(_buffer, value);
        if (_attributeMaxSize != 0) {
            auto truncatedEnd =
                str::UTF8SafeTruncation(_buffer.begin() + before, _buffer.end(), _attributeMaxSize);
            if (truncatedEnd != _buffer.end()) {
                BSONObjBuilder truncationInfo = _truncated.subobjStart(name);
                truncationInfo.append("type"_sd, typeName(BSONType::String));
                truncationInfo.append("size"_sd, static_cast<int64_t>(_buffer.size() - before));
                truncationInfo.done();
            }

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
                           StringData context,
                           StringData message,
                           const TypeErasedAttributeStorage& attrs,
                           LogTag tags,
                           LogTruncation truncation) const {
    namespace c = constants;
    static constexpr auto kFmt = JsonStringFormat::ExtendedRelaxedV2_0_0;
    StringData severityString = severity.toStringDataCompact();
    StringData componentString = component.getNameForLog();
    bool local = _timestampFormat == LogTimestampFormat::kISO8601Local;
    size_t attributeMaxSize = truncation != LogTruncation::Enabled
        ? 0
        : (_maxAttributeSizeKB != 0 ? _maxAttributeSizeKB->loadRelaxed() * 1024
                                    : c::kDefaultMaxAttributeOutputSizeKB * 1024);
    auto write = [&](StringData s) { buffer.append(s.rawData(), s.rawData() + s.size()); };

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

    auto strFn = [&](StringData s) { return [&, s] { write(s); }; };
    auto escFn = [&](StringData s) { return [&, s] { str::escapeForJSON(buffer, s); }; };
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

    auto dateFn = [&](Date_t date) {
        return jsobj([&, date](CommaTracker& tracker) {
            field(tracker, "$date", quote([&, date] {
                      write(StringData{DateStringBuffer{}.iso8601(date, local)});
                  }));
        });
    };
    auto bsonObjFn = [&](BSONObj o) {
        return [&, o] { o.jsonStringBuffer(kFmt, 0, false, buffer, 0); };
    };
    auto bsonArrFn = [&](BSONArray a) {
        return [&, a] { a.jsonStringBuffer(kFmt, 0, true, buffer, 0); };
    };

    jsobj([&](CommaTracker& top) {
        field(top, c::kTimestampFieldName, dateFn(date));
        field(top, c::kSeverityFieldName, padNextComma(top, 5, quote(strFn(severityString))));
        field(top, c::kComponentFieldName, padNextComma(top, 11, quote(strFn(componentString))));
        field(top, c::kIdFieldName, padNextComma(top, 8, intFn(id)));
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
        if (tags != LogTag::kNone)
            field(top, c::kTagsFieldName, bsonArrFn(tags.toBSONArray()));
    })();
}

void JSONFormatter::operator()(boost::log::record_view const& rec,
                               boost::log::formatting_ostream& strm) const {
    using boost::log::extract;

    fmt::memory_buffer buffer;

    format(buffer,
           extract<LogSeverity>(attributes::severity(), rec).get(),
           extract<LogComponent>(attributes::component(), rec).get(),
           extract<Date_t>(attributes::timeStamp(), rec).get(),
           extract<int32_t>(attributes::id(), rec).get(),
           extract<StringData>(attributes::threadName(), rec).get(),
           extract<StringData>(attributes::message(), rec).get(),
           extract<TypeErasedAttributeStorage>(attributes::attributes(), rec).get(),
           extract<LogTag>(attributes::tags(), rec).get(),
           extract<LogTruncation>(attributes::truncation(), rec).get());

    // Write final JSON object to output stream
    strm.write(buffer.data(), buffer.size());
}

}  // namespace mongo::logv2
