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

#include "mongo/util/options_parser/environment.h"
#include "mongo/util/options_parser/option_section.h"

namespace mongo {
namespace optionenvironment {

/*
 * This structure stores information about all the command line options.  The parser will use
 * this description when it parses the command line, the INI config file, and the JSON config
 * file.  See the OptionSection and OptionDescription classes for more details.
 *
 * Example:
 * MONGO_MODULE_STARTUP_OPTIONS_REGISTER(MongodOptions)(InitializerContext* context) {
 *          return addMongodOptions(&moe::startupOptions);
 *     startupOptions.addOptionChaining("option", "option", moe::String, "description");
 *     return Status::OK();
 * }
 */
extern OptionSection startupOptions;

/*
 * This structure stores the parsed command line options.  After the "defult" group of the
 * MONGO_INITIALIZERS, this structure should be fully validated from an option perspective.  See
 * the Environment, Constraint, and Value classes for more details.
 *
 * Example:
 * if (startupOptionsParsed.count("option")) {
 *     std::string value;
 *     ret = startupOptionsParsed.get("option", &value);
 *     if (!ret.isOK()) {
 *         return ret;
 *     }
 * }
 */
extern Environment startupOptionsParsed;

}  // namespace optionenvironment
}  // namespace mongo
