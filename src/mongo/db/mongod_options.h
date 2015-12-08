/*
 *    Copyright (C) 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"

namespace mongo {

namespace optionenvironment {
class OptionSection;
class Environment;
}  // namespace optionenvironment

namespace moe = mongo::optionenvironment;

struct MongodGlobalParams {
    bool scriptingEnabled;  // --noscripting

    MongodGlobalParams() : scriptingEnabled(true) {}
};

extern MongodGlobalParams mongodGlobalParams;

Status addMongodOptions(moe::OptionSection* options);

void printMongodHelp(const moe::OptionSection& options);

/**
 * Handle options that should come before validation, such as "help".
 *
 * Returns false if an option was found that implies we should prematurely exit with success.
 */
bool handlePreValidationMongodOptions(const moe::Environment& params,
                                      const std::vector<std::string>& args);

/**
 * Handle custom validation of mongod options that can not currently be done by using
 * Constraints in the Environment.  See the "validate" function in the Environment class for
 * more details.
 */
Status validateMongodOptions(const moe::Environment& params);

/**
 * Canonicalize mongod options for the given environment.
 *
 * For example, the options "dur", "nodur", "journal", "nojournal", and
 * "storage.journaling.enabled" should all be merged into "storage.journaling.enabled".
 */
Status canonicalizeMongodOptions(moe::Environment* params);

// Must be called after "storeMongodOptions"
StatusWith<repl::ReplSettings> parseMongodReplicationOptions(const moe::Environment& params);

Status storeMongodOptions(const moe::Environment& params, const std::vector<std::string>& args);

void setGlobalReplSettings(const repl::ReplSettings& settings);
const repl::ReplSettings& getGlobalReplSettings();
}
