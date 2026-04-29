/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/util/active_exception_witness.h"

#include "mongo/logv2/redaction.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/demangle.h"

#include <exception>

#include <boost/exception/diagnostic_information.hpp>
#include <boost/exception/exception.hpp>
#include <fmt/format.h>

namespace mongo {

ActiveExceptionWitness::ActiveExceptionWitness() {
    // Later entries in the catch chain will become the innermost catch blocks, so
    // these are in order of increasing specificity. User-provided probes
    // will be appended, so they will be considered more specific than any of
    // these, which are essentially "fallback" handlers.
    addHandler<boost::exception>([](const auto& ex) {
        return ExceptionInfo{
            .description = fmt::format("boost::diagnostic_information(): {}",
                                       boost::diagnostic_information(ex)),
            .type = &typeid(ex),
        };
    });
    addHandler<std::exception>([](const auto& ex) {
        return ExceptionInfo{
            .description = fmt::format("std::exception::what(): {}", redact(ex.what())),
            .type = &typeid(ex),
        };
    });
    addHandler<DBException>([](const auto& ex) {
        return ExceptionInfo{
            .description = fmt::format("DBException::toString(): {}", redact(ex)),
            .type = &typeid(ex),
        };
    });
}

void ActiveExceptionWitness::describe(std::ostream& os) {
    if (auto i = info()) {
        os << i->description << "\n";
        _exceptionTypeBlurb(*i->type, os);
        return;
    }
    os << "An unknown exception was thrown\n";
}

boost::optional<ActiveExceptionWitness::ExceptionInfo> ActiveExceptionWitness::info() {
    invariant(std::current_exception() != nullptr);
    CatchAndDescribe dc;
    for (const auto& config : _configurators)
        config(dc);
    try {
        ExceptionInfo info;
        dc.doCatch(info);
        return info;
    } catch (...) {
        return boost::none;
    }
}

void ActiveExceptionWitness::_exceptionTypeBlurb(const std::type_info& ex, std::ostream& os) {
    os << "Actual exception type: " << demangleName(ex) << "\n";
}

ActiveExceptionWitness& globalActiveExceptionWitness() {
    static StaticImmortal<ActiveExceptionWitness> v;
    return *v;
}

std::string describeActiveException() {
    std::ostringstream oss;
    globalActiveExceptionWitness().describe(oss);
    return oss.str();
}

boost::optional<ActiveExceptionWitness::ExceptionInfo> activeExceptionInfo() {
    return globalActiveExceptionWitness().info();
}

}  // namespace mongo
