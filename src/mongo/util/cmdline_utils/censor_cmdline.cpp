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

#include "mongo/util/cmdline_utils/censor_cmdline.h"

#include <set>
#include <string>

#include "mongo/util/options_parser/startup_option_init.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/str.h"

namespace mongo {
namespace cmdline_utils {

namespace {

struct InsensitiveCompare {
    bool operator()(const std::string& a, const std::string& b) const {
        return str::caseInsensitiveCompare(a.c_str(), b.c_str()) < 0;
    }
};

std::set<std::string, InsensitiveCompare> gRedactedDottedNames;
std::set<std::string, InsensitiveCompare> gRedactedSingleNames;
std::set<char> gRedactedCharacterNames;

bool gGatherOptionsDone = false;
// Gather list of deprecated names from config options.
// Must happen after all settings are registered, but before "StartupOptions" asks for censoring.
MONGO_INITIALIZER_GENERAL(GatherReadctionOptions,
                          ("EndStartupOptionRegistration"),
                          ("BeginStartupOptionSetup"))
(InitializerContext*) {
    const auto insertSingleName = [](const std::string& name) -> Status {
        // Single names may be of the format: "name,n"
        // Split those into single and character names.
        auto pos = name.find(',');
        if (pos == std::string::npos) {
            // Simple case, no comma.
            gRedactedSingleNames.insert(name);
            return Status::OK();
        }
        if (pos != (name.size() - 2)) {
            return {ErrorCodes::BadValue,
                    str::stream() << "Invalid short name for config option: " << name};
        }
        gRedactedSingleNames.insert(name.substr(0, pos));
        gRedactedCharacterNames.insert(name[pos + 1]);
        return Status::OK();
    };

    std::vector<optionenvironment::OptionDescription> options;
    auto status = optionenvironment::startupOptions.getAllOptions(&options);
    if (!status.isOK()) {
        return status;
    }

    for (const auto& opt : options) {
        if (!opt._redact) {
            continue;
        }

        gRedactedDottedNames.insert(opt._dottedName);
        gRedactedDottedNames.insert(opt._deprecatedDottedNames.begin(),
                                    opt._deprecatedDottedNames.end());
        if (!opt._singleName.empty()) {
            auto status = insertSingleName(opt._singleName);
            if (!status.isOK()) {
                return status;
            }
        }
        for (const auto& name : opt._deprecatedSingleNames) {
            auto status = insertSingleName(name);
            if (!status.isOK()) {
                return status;
            }
        }
    }

    gGatherOptionsDone = true;
    return Status::OK();
}

bool _isPasswordArgument(const std::string& name) {
    return gRedactedDottedNames.count(name);
}

bool _isPasswordSwitch(const std::string& name) {
    if ((name.size() < 2) || (name[0] != '-')) {
        return false;
    }

    if ((name.size() == 2) && (gRedactedCharacterNames.count(name[1]))) {
        return true;
    }

    if (gRedactedSingleNames.count(name.substr(1))) {
        // "-option"
        return true;
    }

    if ((name[1] != '-') || (name.size() < 3)) {
        return false;
    }

    // "--option"
    return gRedactedSingleNames.count(name.substr(2));
}

// Used by argv redaction in place.
void _redact(char* arg) {
    for (; *arg; ++arg) {
        *arg = 'x';
    }
}
}  // namespace

void censorBSONObjRecursive(const BSONObj& params,          // Object we are censoring
                            const std::string& parentPath,  // Set if this is a sub object
                            bool isArray,
                            BSONObjBuilder* result) {
    BSONObjIterator paramsIterator(params);
    while (paramsIterator.more()) {
        BSONElement param = paramsIterator.next();
        std::string dottedName =
            (parentPath.empty() ? param.fieldName()
                                : isArray ? parentPath : parentPath + '.' + param.fieldName());
        if (param.type() == Array) {
            BSONObjBuilder subArray(result->subarrayStart(param.fieldName()));
            censorBSONObjRecursive(param.Obj(), dottedName, true, &subArray);
            subArray.done();
        } else if (param.type() == Object) {
            BSONObjBuilder subObj(result->subobjStart(param.fieldName()));
            censorBSONObjRecursive(param.Obj(), dottedName, false, &subObj);
            subObj.done();
        } else if (param.type() == String) {
            if (_isPasswordArgument(dottedName.c_str())) {
                result->append(param.fieldName(), "<password>");
            } else {
                result->append(param);
            }
        } else {
            result->append(param);
        }
    }
}

void censorBSONObj(BSONObj* params) {
    invariant(gGatherOptionsDone);
    BSONObjBuilder builder;
    censorBSONObjRecursive(*params, "", false, &builder);
    *params = builder.obj();
}

void censorArgsVector(std::vector<std::string>* args) {
    invariant(gGatherOptionsDone);
    for (size_t i = 0; i < args->size(); ++i) {
        std::string& arg = args->at(i);
        const std::string::iterator endSwitch = std::find(arg.begin(), arg.end(), '=');
        std::string switchName(arg.begin(), endSwitch);
        if (_isPasswordSwitch(switchName)) {
            if (endSwitch == arg.end()) {
                if (i + 1 < args->size()) {
                    args->at(i + 1) = "<password>";
                }
            } else {
                arg = switchName + "=<password>";
            }
        } else if ((switchName.size() > 2) && _isPasswordSwitch(switchName.substr(0, 2))) {
            // e.g. "-ppassword"
            arg = switchName.substr(0, 2) + "<password>";
        }
    }
}

void censorArgvArray(int argc, char** argv) {
    invariant(gGatherOptionsDone);
    // Algorithm:  For each arg in argv:
    //   Look for an equal sign in arg; if there is one, temporarily nul it out.
    //   check to see if arg is a password switch.  If so, overwrite the value
    //     component with xs.
    //   restore the nul'd out equal sign, if any.
    for (int i = 0; i < argc; ++i) {
        char* const arg = argv[i];
        char* const firstEqSign = strchr(arg, '=');
        if (nullptr != firstEqSign) {
            *firstEqSign = '\0';
        }

        if (_isPasswordSwitch(arg)) {
            if (nullptr == firstEqSign) {
                if (i + 1 < argc) {
                    _redact(argv[i + 1]);
                }
            } else {
                _redact(firstEqSign + 1);
            }
        } else if ((strlen(arg) > 2) && _isPasswordSwitch(std::string(arg, 2))) {
            // e.g. "-ppassword"
            _redact(argv[i] + 2);
        }

        if (nullptr != firstEqSign) {
            *firstEqSign = '=';
        }
    }
}
}  // namespace cmdline_utils
}
