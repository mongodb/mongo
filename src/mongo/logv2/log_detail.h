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

#include "mongo/base/status.h"
#include "mongo/bson/util/builder.h"
#include "mongo/logv2/attribute_argument_set.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_options.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/util/errno_util.h"

namespace mongo {
namespace logv2 {
namespace detail {
void doLogImpl(LogSeverity const& severity,
               LogOptions const& options,
               StringData stable_id,
               StringData message,
               AttributeArgumentSet const& attrs);

void doLogRecordImpl(LogRecord&& debugRecord,
                     LogDomain& domain,
                     StringData message,
                     AttributeArgumentSet const& attrs);

template <typename S, typename... Args>
void doLog(LogSeverity const& severity,
           LogOptions const& options,
           StringData stable_id,
           S const& message,
           fmt::internal::named_arg<Args, char>&&... args) {
    AttributeArgumentSet attr_set;
    auto arg_store = fmt::internal::make_args_checked(message, (args.value)...);
    attr_set._values = arg_store;
    (attr_set._names.push_back(::mongo::StringData(args.name.data(), args.name.size())), ...);
    auto msg = static_cast<fmt::string_view>(message);
    doLogImpl(severity, options, stable_id, ::mongo::StringData(msg.data(), msg.size()), attr_set);
}

template <typename S, typename... Args>
void doLogRecord(LogRecord&& record,
                 LogDomain& domain,
                 S const& message,
                 fmt::internal::named_arg<Args, char>&&... args) {
    AttributeArgumentSet attr_set;
    auto arg_store = fmt::internal::make_args_checked(message, (args.value)...);
    attr_set._values = arg_store;
    (attr_set._names.push_back(::mongo::StringData(args.name.data(), args.name.size())), ...);
    auto msg = static_cast<fmt::string_view>(message);
    doLogRecordImpl(
        std::move(record), domain, ::mongo::StringData(msg.data(), msg.size()), attr_set);
}

}  // namespace detail
}  // namespace logv2

inline fmt::internal::udl_arg<char> operator"" _attr(const char* s, std::size_t) {
    return {s};
}

}  // namespace mongo
