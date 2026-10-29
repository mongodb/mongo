// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/str.h"

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo {

/**
 * A single named rule about a state transition. `check` returns OK if the rule holds, otherwise an
 * error whose reason describes the violation including the offending values.
 */
template <typename Input>
struct TransitionConstraint {
    std::string_view name;
    std::function<Status(const Input&)> check;
};

/**
 * Outcome of running constraints for one transition. `failed` lists the names of the violated
 * constraints in the order they were checked, so tests can assert on them.
 */
struct ConstraintResult {
    Status status;
    std::vector<std::string> failed;
};

/**
 * Accumulates constraint failures for one transition. Constraint definitions live once, next to
 * their names; call sites chain the ones that apply to their edge. The input is stored by value;
 * any documents it references must outlive the full expression.
 */
template <typename Input>
class TransitionCheck {
public:
    TransitionCheck(std::string edge, Input in, ErrorCodes::Error code = ErrorCodes::InternalError)
        : _edge(std::move(edge)), _in(std::move(in)), _code(code) {}

    TransitionCheck& require(const TransitionConstraint<Input>& constraint) {
        auto status = constraint.check(_in);
        if (!status.isOK()) {
            _failures.emplace_back(std::string{constraint.name}, std::move(status));
        }
        return *this;
    }

    ConstraintResult result() const {
        if (_failures.empty()) {
            return {Status::OK(), {}};
        }

        std::string violations;
        std::vector<std::string> failed;
        for (const auto& [name, status] : _failures) {
            if (!violations.empty()) {
                violations += ", ";
            }
            violations += std::string(str::stream() << name << " (" << status.reason() << ")");
            failed.emplace_back(name);
        }
        return {Status{_code, str::stream() << _edge << " violated: " << violations},
                std::move(failed)};
    }

private:
    std::string _edge;
    Input _in;
    ErrorCodes::Error _code;
    std::vector<std::pair<std::string, Status>> _failures;
};

template <typename StateEnum>
std::string describeTransitionEdge(StateEnum from, StateEnum to) {
    return str::stream() << "transition from " << idl::serialize(from) << " to "
                         << idl::serialize(to);
}

}  // namespace mongo
