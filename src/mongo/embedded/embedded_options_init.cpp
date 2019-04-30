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

#include "mongo/embedded/embedded_options.h"

#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {
namespace embedded {

MONGO_GENERAL_STARTUP_OPTIONS_REGISTER(EmbeddedOptions)(InitializerContext* context) {
    return addOptions(&optionenvironment::startupOptions);
}

GlobalInitializerRegisterer embeddedOptionsInitializer(
    "EmbeddedOptions",
    [](InitializerContext* context) {
        // Run validation, but tell the Environment that we don't want it to be set as "valid",
        // since we may be making it invalid in the canonicalization process.
        Status ret = optionenvironment::startupOptionsParsed.validate(false);
        if (!ret.isOK()) {
            return ret;
        }
        ret = canonicalizeOptions(&optionenvironment::startupOptionsParsed);
        if (!ret.isOK()) {
            return ret;
        }
        ret = optionenvironment::startupOptionsParsed.validate();
        if (!ret.isOK()) {
            return ret;
        }
        return Status::OK();
    },
    [](DeinitializerContext* context) { return Status::OK(); },
    {"BeginStartupOptionValidation", "AllFailPointsRegistered"},
    {"EndStartupOptionValidation"});

GlobalInitializerRegisterer embeddedOptionsStore("EmbeddedOptions_Store",
                                                 [](InitializerContext* context) {
                                                     return storeOptions(
                                                         optionenvironment::startupOptionsParsed);
                                                 },
                                                 [](DeinitializerContext* context) {
                                                     resetOptions();
                                                     return Status::OK();
                                                 },
                                                 {"BeginStartupOptionStorage"},
                                                 {"EndStartupOptionStorage"});

}  // namespace embedded
}  // namespace mongo
