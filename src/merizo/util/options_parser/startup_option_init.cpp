/**
 *    Copyright (C) 2018-present MerizoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MerizoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.merizodb.com/licensing/server-side-public-license>.
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

#include "merizo/util/options_parser/startup_option_init.h"

/*
 * These are the initializer groups for command line and config file option registration, parsing,
 * validation, and storage
 */

/* Groups for all of option handling */
MERIZO_INITIALIZER_GROUP(BeginStartupOptionHandling,
                        ("GlobalLogManager", "ValidateLocale"),
                        ("EndStartupOptionHandling"))

/* Groups for option registration */
MERIZO_INITIALIZER_GROUP(BeginStartupOptionRegistration,
                        ("BeginStartupOptionHandling"),
                        ("EndStartupOptionRegistration"))

/* Groups for general option registration (useful for controlling the order in which options are
 * registered for modules, which affects the order in which they are printed in help output) */
MERIZO_INITIALIZER_GROUP(BeginGeneralStartupOptionRegistration,
                        ("BeginStartupOptionRegistration"),
                        ("EndGeneralStartupOptionRegistration"))
MERIZO_INITIALIZER_GROUP(EndGeneralStartupOptionRegistration,
                        ("BeginGeneralStartupOptionRegistration"),
                        ("EndStartupOptionRegistration"))

MERIZO_INITIALIZER_GROUP(EndStartupOptionRegistration,
                        ("BeginStartupOptionRegistration"),
                        ("BeginStartupOptionParsing"))

/* Groups for option parsing */
MERIZO_INITIALIZER_GROUP(BeginStartupOptionParsing,
                        ("EndStartupOptionRegistration"),
                        ("EndStartupOptionParsing"))
MERIZO_INITIALIZER_GROUP(EndStartupOptionParsing,
                        ("BeginStartupOptionParsing"),
                        ("BeginStartupOptionValidation"))

/* Groups for option validation */
MERIZO_INITIALIZER_GROUP(BeginStartupOptionValidation,
                        ("EndStartupOptionParsing"),
                        ("EndStartupOptionValidation"))
MERIZO_INITIALIZER_GROUP(EndStartupOptionValidation,
                        ("BeginStartupOptionValidation"),
                        ("BeginStartupOptionSetup"))

/* Groups for option setup */
MERIZO_INITIALIZER_GROUP(BeginStartupOptionSetup,
                        ("EndStartupOptionValidation"),
                        ("EndStartupOptionSetup"))
MERIZO_INITIALIZER_GROUP(EndStartupOptionSetup,
                        ("BeginStartupOptionSetup"),
                        ("BeginStartupOptionStorage"))

/* Groups for option storage */
MERIZO_INITIALIZER_GROUP(BeginStartupOptionStorage,
                        ("EndStartupOptionSetup"),
                        ("EndStartupOptionStorage"))
MERIZO_INITIALIZER_GROUP(EndStartupOptionStorage,
                        ("BeginStartupOptionStorage"),
                        ("BeginPostStartupOptionStorage"))

/* Groups for post option storage */
MERIZO_INITIALIZER_GROUP(BeginPostStartupOptionStorage,
                        ("EndStartupOptionStorage"),
                        ("EndPostStartupOptionStorage"))
MERIZO_INITIALIZER_GROUP(EndPostStartupOptionStorage,
                        ("BeginPostStartupOptionStorage"),
                        ("EndStartupOptionHandling"))

MERIZO_INITIALIZER_GROUP(EndStartupOptionHandling, ("BeginStartupOptionHandling"), ("default"))
