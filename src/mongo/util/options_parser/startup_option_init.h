/*    Copyright 2013 10gen Inc.
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
 * Utility macros for declaring global initializers for startup options.
 *
 * Should NOT be included by other header files.  Include only in source files.
 *
 * Initializer functions take a parameter of type ::mongo::InitializerContext*, and return
 * a Status.  Any status other than Status::OK() is considered a failure that will stop further
 * intializer processing.
 *
 * Note that currently storage and validation are done in the same stage, so do not try to do things
 * that depend on your initializer being between these two stages
 *
 * See comments in the included init.h file for more details.
 */

#pragma once

#include "mongo/base/init.h"

/**
 * Helper macros to use when handling startup options
 * Note that you should only need these if you are working with options or are using an initializer
 * before the "default" group, which is set to happen after all the option initialization is done
 */

/**
 * Macro to define an initializer function named "<fname>_Register" to register general startup
 * options that are always in the executable
 *
 * Usage:
 *     MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(MongodOptions)(InitializerContext* context) {
 *         return Status::OK();
 *     }
 */
#define MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(fname) \
    MONGO_INITIALIZER_GENERAL(fname##_Register, \
                              ("BeginGeneralStartupOptionRegistration"), \
                              ("EndGeneralStartupOptionRegistration"))

/**
 * Macro to define an initializer function named "<fname>_Register" to register module startup
 * options that are conditionally linked into the executable.  This will get run after the general
 * startup options, and thus options registered in these will appear after those in help output.
 *
 * Usage:
 *     MONGO_MODULE_STARTUP_OPTIONS_REGISTER(MongodOptions)(InitializerContext* context) {
 *         return Status::OK();
 *     }
 */
#define MONGO_MODULE_STARTUP_OPTIONS_REGISTER(fname) \
    MONGO_INITIALIZER_GENERAL(fname##_Register, \
                              ("EndGeneralStartupOptionRegistration"), \
                              ("EndStartupOptionRegistration"))

/**
 * Macro to define an initializer function named "<fname>_Parse" to parse the command line and
 * config file options.
 * This should only be defined once per binary.
 *
 * Usage:
 *     MONGO_STARTUP_OPTIONS_PARSE(MongodOptions)(InitializerContext* context) {
 *         return Status::OK();
 *     }
 */
#define MONGO_STARTUP_OPTIONS_PARSE(fname) \
    MONGO_INITIALIZER_GENERAL(fname##_Parse, \
                              ("BeginStartupOptionParsing"), \
                              ("EndStartupOptionParsing"))

/**
 * Macro to define an initializer function named "<fname>_Validate" to validate the command line and
 * config file options.
 *
 * Warning:  Validation still currently happens in the storage stage, so do not write an initializer
 * that depends on them being separate.
 *
 * Usage:
 *     MONGO_STARTUP_OPTIONS_PARSE(MongodOptions)(InitializerContext* context) {
 *         return Status::OK();
 *     }
 */
#define MONGO_STARTUP_OPTIONS_VALIDATE(fname) \
    MONGO_INITIALIZER_GENERAL(fname##_Validate, \
                              ("BeginStartupOptionValidation"), \
                              ("EndStartupOptionValidation"))

/**
 * Macro to define an initializer function named "<fname>_Store" to store the command line and
 * config file options.  This includes setting the values of global variables and configuring global
 * state.
 *
 * Warning:  Validation still currently happens in the storage stage, so do not write an initializer
 * that depends on them being separate.
 *
 * Usage:
 *     MONGO_STARTUP_OPTIONS_PARSE(MongodOptions)(InitializerContext* context) {
 *         return Status::OK();
 *     }
 */
#define MONGO_STARTUP_OPTIONS_STORE(fname) \
    MONGO_INITIALIZER_GENERAL(fname##_Store, \
                              ("BeginStartupOptionStorage"), \
                              ("EndStartupOptionStorage"))
