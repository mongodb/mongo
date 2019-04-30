/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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

#include "mongo/base/init.h"

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
