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

#include "mongo/util/options_parser/startup_option_init.h"

/*
 * These are the initializer groups for command line and config file option registration, parsing,
 * validation, and storage
 */

/* Groups for all of option handling */
MONGO_INITIALIZER_GROUP(BeginStartupOptionHandling,
                        ("GlobalLogManager"), ("EndStartupOptionHandling"))

/* Groups for option registration */
MONGO_INITIALIZER_GROUP(BeginStartupOptionRegistration,
                        ("BeginStartupOptionHandling"), ("EndStartupOptionRegistration"))

/* Groups for general option registration (useful for controlling the order in which options are
 * registered for modules, which affects the order in which they are printed in help output) */
MONGO_INITIALIZER_GROUP(BeginGeneralStartupOptionRegistration,
                        ("BeginStartupOptionRegistration"), ("EndGeneralStartupOptionRegistration"))
MONGO_INITIALIZER_GROUP(EndGeneralStartupOptionRegistration,
                        ("BeginGeneralStartupOptionRegistration"), ("EndStartupOptionRegistration"))

MONGO_INITIALIZER_GROUP(EndStartupOptionRegistration,
                        ("BeginStartupOptionRegistration"), ("BeginStartupOptionParsing"))

/* Groups for option parsing */
MONGO_INITIALIZER_GROUP(BeginStartupOptionParsing,
                        ("EndStartupOptionRegistration"), ("EndStartupOptionParsing"))
MONGO_INITIALIZER_GROUP(EndStartupOptionParsing,
                        ("BeginStartupOptionParsing"), ("BeginStartupOptionValidation"))

/* Groups for option validation */
MONGO_INITIALIZER_GROUP(BeginStartupOptionValidation,
                        ("EndStartupOptionParsing"), ("EndStartupOptionValidation"))
MONGO_INITIALIZER_GROUP(EndStartupOptionValidation,
                        ("BeginStartupOptionValidation"), ("BeginStartupOptionStorage"))

/* Groups for option storage */
MONGO_INITIALIZER_GROUP(BeginStartupOptionStorage,
                        ("EndStartupOptionValidation"), ("EndStartupOptionStorage"))
MONGO_INITIALIZER_GROUP(EndStartupOptionStorage,
                        ("BeginStartupOptionStorage"), ("EndStartupOptionHandling"))

MONGO_INITIALIZER_GROUP(EndStartupOptionHandling, ("BeginStartupOptionHandling"), ("default"))
