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

#pragma once

#include <boost/function.hpp>
#include <string>
#include <typeinfo>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/platform/unordered_map.h"

namespace mongo {

    /**
     * Utility class for setting and getting the values of configuration variables.
     *
     * A common kind of global variable is a configuration setting for a module or set of modules.
     * The primary purpose of this class is to generically expose these settings so that they may be
     * configured at program startup (say by inspecting argv, the environment or config files).  The
     * secondary purpose is to provide a facility for introspection on these settings, for use by
     * reporting utilities built into a mongo application.
     *
     * By providing modules with access to an instance of this variable during a registration phase
     * of the application, those modules may register their global variables with unique names,
     * using "registerVariable<T>()".  Then, during a configuration phase, the application may set
     * values for configurables to non-default values using "setVariable()".
     *
     * After the configuration phase, modules may directly access their registered variables via the
     * storage they provided at registration time; there is no need to maintain access to the
     * configuration manager at all.
     *
     * Instances of this class provide no facilities for coordinating activity among threads, so in
     * a multi-threaded scenario, it is the responsibility of the programmer to ensure that modules
     * do not access configuration variables from separate threads before the configuration phase
     * completes.  The easiest way to accomplish this is to perform configuration in a
     * single-threaded context.
     *
     * TODO: Support introspection of configuration variables, for use in reporting tools, etc.
     */
    class ConfigurationVariableManager {
        MONGO_DISALLOW_COPYING(ConfigurationVariableManager);

    public:
        typedef boost::function<Status (const std::string&)> SetFromStringFn;

        ConfigurationVariableManager();
        ~ConfigurationVariableManager();

        /**
         * Register a variable named "name" whose value may be set from a string
         * using the function-like object "setter".
         *
         * Returns ErrorCodes::DuplicateKey if another module has already registered "name",
         * or ErrorCodes::BadValue if "setter" is an invalid function object.
         */
        Status registerVariableFn(const std::string& name, const SetFromStringFn setter);

        /**
         * Register a variable of type "T", named "name", stored at "storage".
         *
         * Returns ErrorCodes::DuplicateKey if another module has already registered "name",
         * or ErrorCodes::BadValue if "storage" is NULL.
         *
         * Uses a generic SetFromString function, based on the type T.
         */
        template <typename T>
        Status registerVariable(const std::string& name, T* storage) {
            if (!storage)
                return Status(ErrorCodes::BadValue, "Storage is null");
            return registerVariableFn(name, SetFromStringImpl<T>(storage));
        }

        /**
         * Set the variable named "name" to the value "value".
         *
         * If "name" was not previously added, returns ErrorCodes::NoSuchKey.
         *
         * Returns Status::OK() and sets the named variable to the parsed value of "value" on
         * success, and returns the error from the associated SetFromStringFn on failure.
         */
        Status setVariable(const std::string& name, const std::string& value) const;

    private:

        typedef unordered_map<std::string, SetFromStringFn> VariableMap;

        /**
         * Template type of generic set-from-string function objects, used in the
         * registerVariable<T> method, above.  Available implementations can
         * be seen in configuration_variable_manager.cpp.
         */
        template <typename T>
        class SetFromStringImpl {
        public:
            explicit SetFromStringImpl(T* storage) : _storage(storage) {}
            Status operator()(const std::string& stringValue) const;

        private:
            T* _storage;
        };


        /// Map from names of registered variables to their storage location and type information.
        VariableMap _variables;
    };

}  // namespace mongo
