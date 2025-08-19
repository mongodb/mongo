
/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "CustomAttributes.hh"
#include "Exception.hh"
#include <map>
#include <memory>

namespace avro {

boost::optional<std::string> CustomAttributes::getAttribute(const std::string &name) const {
    boost::optional<std::string> result;
    std::map<std::string, std::string>::const_iterator iter =
        attributes_.find(name);
    if (iter == attributes_.end()) {
        return result;
    }
    result = iter->second;
    return result;
}

void CustomAttributes::addAttribute(const std::string &name,
                                    const std::string &value) {
    auto iter_and_find =
        attributes_.insert(std::pair<std::string, std::string>(name, value));
    if (!iter_and_find.second) {
        throw Exception(name + " already exists and cannot be added");
    }
}

void CustomAttributes::printJson(std::ostream &os,
                                 const std::string &name) const {
    if (attributes().find(name) == attributes().end()) {
        throw Exception(name + " doesn't exist");
    }
    os << "\"" << name << "\": \"" << attributes().at(name) << "\"";
}
} // namespace avro
