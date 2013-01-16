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

/**
 * Utility macros for declaring global initializers and configurable variables.
 *
 * Should NOT be included by other header files.  Include only in source files.
 *
 * Initializers are arranged in an acyclic directed dependency graph.  Declaring
 * a cycle will lead to a runtime error.
 *
 * Initializer functions take a parameter of type ::mongo::InitializerContext*, and return
 * a Status.  Any status other than Status::OK() is considered a failure that will stop further
 * intializer processing.
 *
 * Global configuration variables are declared and set using initializers and a few groups.  All
 * global configuration declarations have "globalVariableConfigurationStarted" as a prerequisite,
 * and "globalVariablesDeclared" as a dependent.  The easiest way for programs to then configure
 * those values to non-default settings is to use the MONGO_CONFIG_VARIABLE_SETTER macro
 * to declare exactly one function that has "globalVariablesDeclared" as a prerequisite and
 * "globalVariablesSet" as a dependent.
 *
 * Initializers that wish to use configurable global variables must have "globalVariablesConfigured"
 * as a direct or indirect prerequisite.  The "default" prerequisite depends on
 * "globalVariablesConfigured", so most initializer functions can safely use global configurable
 * variables.
 *
 * Programmers may validate global variables after they are set using an initializer declared as
 * MONGO_CONFIG_VARIABLE_VALIDATOR, which has "globalVariablesSet" as prerequisite and
 * "globalVariablesConfigured" as dependent.
 *
 * In summary, the following partial order is provided:
 *    All MONGO_CONFIG_VARIABLE_REGISTER()s are evaluated before
 *    The MONGO_CONFIG_VARIABLE_SETTER is evaluated before
 *    All MONGO_CONFIG_VARIABLE_VALIDATORs are evaluated before
 *    Things dependent on "default" are evaluated.
 */

#pragma once

#include "mongo/base/configuration_variable_manager.h"
#include "mongo/base/initializer.h"
#include "mongo/base/initializer_context.h"
#include "mongo/base/initializer_function.h"
#include "mongo/base/global_initializer.h"
#include "mongo/base/global_initializer_registerer.h"
#include "mongo/base/make_string_vector.h"
#include "mongo/base/status.h"

/**
 * Convenience parameter representing an empty set of prerequisites for an initializer function.
 */
#define MONGO_NO_PREREQUISITES ()

/**
 * Convenience parameter representing an empty set of dependents of an initializer function.
 */
#define MONGO_NO_DEPENDENTS ()

/**
 * Convenience parameter representing the default set of dependents for initializer functions.
 */
#define MONGO_DEFAULT_PREREQUISITES ("default")

/**
 * Macro to define an initializer function named "NAME" with the default prerequisites, and
 * no explicit dependents.
 *
 * See MONGO_INITIALIZER_GENERAL.
 *
 * Usage:
 *     MONGO_INITIALIZER(myModule)(::mongo::InitializerContext* context) {
 *         ...
 *     }
 */
#define MONGO_INITIALIZER(NAME) \
    MONGO_INITIALIZER_WITH_PREREQUISITES(NAME, MONGO_DEFAULT_PREREQUISITES)

/**
 * Macro to define an initializer function named "NAME" that depends on the initializers
 * specified in PREREQUISITES to have been completed, but names no explicit dependents.
 *
 * See MONGO_INITIALIZER_GENERAL.
 *
 * Usage:
 *     MONGO_INITIALIZER_WITH_PREREQUISITES(myGlobalStateChecker,
 *                                         ("globalStateInitialized", "stacktraces"))(
 *            ::mongo::InitializerContext* context) {
 *    }
 */
#define MONGO_INITIALIZER_WITH_PREREQUISITES(NAME, PREREQUISITES) \
    MONGO_INITIALIZER_GENERAL(NAME, PREREQUISITES, MONGO_NO_DEPENDENTS)

/**
 * Macro to define an initializer that depends on PREREQUISITES and has DEPENDENTS as explicit
 * dependents.
 *
 * NAME is any legitimate name for a C++ symbol.
 * PREREQUISITES is a tuple of 0 or more string literals, i.e., ("a", "b", "c"), or ()
 * DEPENDENTS is a tuple of 0 or more string literals.
 *
 * At run time, the full set of prerequisites for NAME will be computed as the union of the
 * explicit PREREQUISITES and the set of all other mongo initializers that name NAME in their
 * list of dependents.
 *
 * Usage:
 *    MONGO_INITIALIZER_GENERAL(myInitializer,
 *                             ("myPrereq1", "myPrereq2", ...),
 *                             ("myDependent1", "myDependent2", ...))(
 *            ::mongo::InitializerContext* context) {
 *    }
 *
 * TODO: May want to be able to name the initializer separately from the function name.
 * A form that takes an existing function or that lets the programmer supply the name
 * of the function to declare would be options.
 */
#define MONGO_INITIALIZER_GENERAL(NAME, PREREQUISITES, DEPENDENTS) \
    ::mongo::Status _MONGO_INITIALIZER_FUNCTION_NAME(NAME)(::mongo::InitializerContext*); \
    namespace {                                                         \
        ::mongo::GlobalInitializerRegisterer _mongoInitializerRegisterer_##NAME( \
                #NAME,                                                  \
                _MONGO_INITIALIZER_FUNCTION_NAME(NAME),                 \
                MONGO_MAKE_STRING_VECTOR PREREQUISITES,                 \
                MONGO_MAKE_STRING_VECTOR DEPENDENTS);                   \
    }                                                                   \
    ::mongo::Status _MONGO_INITIALIZER_FUNCTION_NAME(NAME)

/**
 * Macro to define an initializer group.
 *
 * An initializer group is an initializer that performs no actions.  It is useful for organizing
 * initialization steps into phases, such as "all global parameter declarations completed", "all
 * global parameters initialized".
 */
#define MONGO_INITIALIZER_GROUP(NAME, PREREQUISITES, DEPENDENTS)        \
    MONGO_INITIALIZER_GENERAL(NAME, PREREQUISITES, DEPENDENTS)(         \
            ::mongo::InitializerContext*) { return ::mongo::Status::OK(); }


/**
 * Macro to register a configurable global variable.
 *
 * "NAME" is the string name through which the variable's storage may be accessed
 * in the ConfigurationVariableManager supplied as part of the InitializerContext
 * to global initializer functions.  "STORAGE" is a pointer to the location in
 * memory where the variable is stored, and "DEFAULT_VALUE" is the value to be
 * assigned as the default, at registration time (once main has started).  This
 * allows DEFAULT_VALUE to be constructed after main() begins, so some options
 * that are not available to static initializers may be available here.
 */
#define MONGO_CONFIG_VARIABLE_REGISTER(NAME, STORAGE, DEFAULT_VALUE)      \
    MONGO_INITIALIZER_GENERAL(cvr_##NAME,                               \
                              ("globalVariableConfigurationStarted"), \
                              ("globalVariablesDeclared"))(             \
                                      ::mongo::InitializerContext* context) { \
        *(STORAGE) = (DEFAULT_VALUE);                                   \
        return ::mongo::getGlobalInitializer().getConfigurationVariableManager().registerVariable( \
                #NAME, (STORAGE));                                      \
    }

/**
 * Convenience macro for functions that validate already-set values of global
 * variables.  Run after the MONGO_CONFIG_VARIABLE_SETTER completes.
 */
#define MONGO_CONFIG_VARIABLE_VALIDATOR(NAME) \
    MONGO_INITIALIZER_GENERAL(NAME, ("globalVariablesConfigured"), ("default"))

/**
 * Convenience macro for declaring the global variable setting function.
 */
#define MONGO_CONFIG_VARIABLE_SETTER                                    \
    MONGO_INITIALIZER_GENERAL(globalVariableSetter,                     \
                              ("globalVariablesDeclared"),              \
                              ("globalVariablesSet"))

/**
 * Macro to produce a name for a mongo initializer function for an initializer operation
 * named "NAME".
 */
#define _MONGO_INITIALIZER_FUNCTION_NAME(NAME) _mongoInitializerFunction_##NAME
