/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
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
