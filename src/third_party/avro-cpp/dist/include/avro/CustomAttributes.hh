/*
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

#ifndef avro_CustomAttributes_hh__
#define avro_CustomAttributes_hh__

#include "Config.hh"
#include <boost/optional.hpp>
#include <iostream>
#include <map>
#include <string>

namespace avro {

// CustomAttributes class stores avro custom attributes.
// Each attribute is represented by a unique name and value.
// User is supposed to create CustomAttributes object and then add it to Schema.
class AVRO_DECL CustomAttributes {
public:
    // Retrieves the custom attribute json entity for that attributeName, returns an
    // null if the attribute doesn't exist.
    boost::optional<std::string> getAttribute(const std::string &name) const;

    // Adds a custom attribute. If the attribute already exists, throw an exception.
    void addAttribute(const std::string &name, const std::string &value);

    // Provides a way to iterate over the custom attributes or check attribute size.
    const std::map<std::string, std::string> &attributes() const {
        return attributes_;
    }

    // Prints the attribute value for the specific attribute.
    void printJson(std::ostream &os, const std::string &name) const;

private:
    std::map<std::string, std::string> attributes_;
};

} // namespace avro

#endif
