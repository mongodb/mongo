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

#include "mongo/base/configuration_variable_manager.h"

#include <boost/bind.hpp>

#include "mongo/base/parse_number.h"

namespace mongo {

    ConfigurationVariableManager::ConfigurationVariableManager() {}

    ConfigurationVariableManager::~ConfigurationVariableManager() {}

    Status ConfigurationVariableManager::registerVariableFn(const std::string& name,
                                                            const SetFromStringFn setter) {
        if (!setter)
            return Status(ErrorCodes::BadValue, "setter function invalid");

        SetFromStringFn& existingSetter = _variables[name];
        if (existingSetter)
            return Status(ErrorCodes::DuplicateKey, name);

        existingSetter = setter;
        return Status::OK();
    }

    Status ConfigurationVariableManager::setVariable(const std::string& name,
                                                     const std::string& value) const {
        const VariableMap::const_iterator iter = _variables.find(name);
        if (_variables.end() == iter)
            return Status(ErrorCodes::NoSuchKey, name);

        return iter->second(value);
    }

    template <>
    Status ConfigurationVariableManager::SetFromStringImpl<std::string>::operator()(
            const std::string& stringValue) const {
        *_storage = stringValue;
        return Status::OK();
    }

    template <typename T>
    Status ConfigurationVariableManager::SetFromStringImpl<T>::operator()(
            const std::string& stringValue) const {

        return parseNumberFromString(stringValue, _storage);
    }

    template class ConfigurationVariableManager::SetFromStringImpl<short>;
    template class ConfigurationVariableManager::SetFromStringImpl<int>;
    template class ConfigurationVariableManager::SetFromStringImpl<long>;
    template class ConfigurationVariableManager::SetFromStringImpl<unsigned short>;
    template class ConfigurationVariableManager::SetFromStringImpl<unsigned int>;
    template class ConfigurationVariableManager::SetFromStringImpl<unsigned long>;

    template <>
    Status ConfigurationVariableManager::SetFromStringImpl<bool>::operator()(
            const std::string& stringValue) const {
        if (stringValue == "true") {
            *_storage = true;
            return Status::OK();
        }
        if (stringValue == "false") {
            *_storage = false;
            return Status::OK();
        }
        return Status(ErrorCodes::FailedToParse,
                      "Could not parse boolean value out of \"" + stringValue + "\"");
    }

}  // namespace mongo
