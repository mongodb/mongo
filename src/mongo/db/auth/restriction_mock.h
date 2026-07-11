// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/auth/restriction.h"

#include <string>

namespace mongo {

class RestrictionMock : public UnnamedRestriction {
public:
    explicit RestrictionMock(bool shouldPass) : _shouldPass(shouldPass) {}

    Status validate(const RestrictionEnvironment& environment) const final {
        if (_shouldPass) {
            return Status::OK();
        }

        return Status(ErrorCodes::AuthenticationRestrictionUnmet,
                      "Mock restriction forced to be unmet");
    }

    void appendToBuilder(BSONArrayBuilder* builder) const override {
        builder->append(_shouldPass);
    }

private:
    void serialize(std::ostream& os) const final {
        os << "{Mock: " << (_shouldPass ? "alwaysMet" : "alwaysUnmet") << "}";
    }

    const bool _shouldPass;
};

class NamedRestrictionMock : public NamedRestriction {
public:
    NamedRestrictionMock(const std::string& name, bool shouldPass)
        : _name(name), _shouldPass(shouldPass) {}

    Status validate(const RestrictionEnvironment& environment) const final {
        if (_shouldPass) {
            return Status::OK();
        }

        return Status(ErrorCodes::AuthenticationRestrictionUnmet,
                      "Mock restriction forced to be unmet");
    }

    void appendToBuilder(BSONObjBuilder* builder) const final {
        builder->append(_name, _shouldPass);
    }

private:
    void serialize(std::ostream& os) const final {
        os << "{" << _name << ": " << (_shouldPass ? "alwaysMet" : "alwaysUnmet") << "}";
    }

    const std::string _name;
    const bool _shouldPass;
};

}  // namespace mongo
