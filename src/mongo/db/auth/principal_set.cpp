/**
*    Copyright (C) 2012 10gen Inc.
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
*/

#include "mongo/pch.h"

#include "mongo/db/auth/principal_set.h"

#include <boost/date_time/posix_time/posix_time.hpp>
#include <string>

#include "mongo/base/status.h"
#include "mongo/db/auth/principal.h"
#include "mongo/platform/unordered_map.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    PrincipalSet::PrincipalSet() {}
    PrincipalSet::~PrincipalSet() {
        for (unordered_map<std::string, Principal*>::iterator it = _principals.begin();
                it != _principals.end(); ++it) {
            delete it->second;
        }
    }

    void PrincipalSet::add(Principal* principal) {
        unordered_map<std::string, Principal*>::iterator it =
                _principals.find(principal->getName());
        if (it != _principals.end()) {
            delete it->second;
        }
        _principals[principal->getName()] = principal;
    }

    Status PrincipalSet::removeByName(const std::string& name) {
        unordered_map<std::string, Principal*>::iterator it = _principals.find(name);
        if (it == _principals.end()) {
            return Status(ErrorCodes::NoSuchKey,
                          mongoutils::str::stream() << "No matching principle found with name: "
                                                    << name,
                          0);
        }
        delete it->second;
        _principals.erase(it);
        return Status::OK();
    }

    Principal* PrincipalSet::lookup(const std::string& name) const {
        unordered_map<std::string, Principal*>::const_iterator it = _principals.find(name);
        if (it == _principals.end()) {
            return NULL;
        }

        return it->second;
    }

} // namespace mongo
