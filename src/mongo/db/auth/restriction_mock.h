/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/db/auth/restriction.h"

#include <string>

#include "mongo/base/status.h"

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

    virtual void appendToBuilder(BSONArrayBuilder* builder) const {
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

    virtual void appendToBuilder(BSONObjBuilder* builder) const final {
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
