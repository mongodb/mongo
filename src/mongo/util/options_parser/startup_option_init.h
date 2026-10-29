// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * Utility macros for declaring global initializers for startup options.
 *
 * Should NOT be included by other header files. Include only in source files.
 *
 * Initializer functions take a parameter of type InitializerContext*, and return
 * a Status. Any status other than Status::OK() is considered a failure that will stop further
 * intializer processing. See src/mongo/base/init.h for details.
 *
 * Note that currently storage and validation are done in the same stage, so do not try to do things
 * that depend on your initializer being between these two stages
 *
 * Note that you should only need these if you are working with options or are using an initializer
 * before the "default" group, which is set to happen after all the option initialization is done.
 *
 * All of these use the same syntax. A typical example of usage:
 *
 *     MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(MongodOptions)(InitializerContext* context) {
 *         return addMongodOptions(&moe::startupOptions);
 *     }
 *
 * MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(fname):
 *     Produces: "<fname>_Register".
 *     Registers general startup options that are always in the executable.
 *
 * MONGO_MODULE_STARTUP_OPTIONS_REGISTER(fname):
 *     Produces: "<fname>_Register".
 *     Registers module startup options that are conditionally linked into the executable.
 *     This will get run after the general startup options, and thus options registered in these
 *     will appear after those in help output.
 *
 * MONGO_STARTUP_OPTIONS_PARSE(fname):
 *     Produces: "<fname>_Parse".
 *     Defines an initializer function to parse the command line and config file options. This
 *     should only be defined once per binary.
 *
 * MONGO_STARTUP_OPTIONS_VALIDATE(fname):
 *     Produces: "<fname>_Validate".
 *     Defines an initializer function to validate the command line and config file options.
 *     Warning:  Validation still currently happens in the storage stage, so do not write an
 *     initializer that depends on them being separate.
 *
 * MONGO_STARTUP_OPTIONS_STORE(fname):
 *     Produces: "<fname>_Store".
 *     Defines an initializer function to store the command line and config file options.  This
 *     includes setting the values of global variables and configuring global state.
 *     Warning:  Validation still currently happens in the storage stage, so do not write an
 *     initializer that depends on them being separate.
 *
 * MONGO_STARTUP_OPTIONS_POST(fname):
 *     Produces: "<fname>_Post".
 *     Defines an initializer function to set internal values after storing the command line and
 *     config file options. This includes setting the values of global variables and configuring
 *     global state.
 */

#pragma once

#include "mongo/base/init.h"  // IWYU pragma: keep

#define MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(fname) \
    MONGO_STARTUP_OPTION_IN_GROUP_(fname, _Register, GeneralStartupOptionRegistration)
#define MONGO_MODULE_STARTUP_OPTIONS_REGISTER(fname) \
    MONGO_STARTUP_OPTION_IN_GROUP_(fname, _Register, ModuleStartupOptionRegistration)
#define MONGO_STARTUP_OPTIONS_PARSE(fname) \
    MONGO_STARTUP_OPTION_IN_GROUP_(fname, _Parse, StartupOptionParsing)
#define MONGO_STARTUP_OPTIONS_VALIDATE(fname) \
    MONGO_STARTUP_OPTION_IN_GROUP_(fname, _Validate, StartupOptionValidation)
#define MONGO_STARTUP_OPTIONS_STORE(fname) \
    MONGO_STARTUP_OPTION_IN_GROUP_(fname, _Store, StartupOptionStorage)
#define MONGO_STARTUP_OPTIONS_POST(fname) \
    MONGO_STARTUP_OPTION_IN_GROUP_(fname, _Post, PostStartupOptionStorage)

#define MONGO_STARTUP_OPTION_IN_GROUP_(fname, suffix, group) \
    MONGO_INITIALIZER_GENERAL(fname##suffix, ("Begin" #group), ("End" #group))
