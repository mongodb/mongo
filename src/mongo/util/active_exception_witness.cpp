// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
