// cmdline.cpp

/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/util/cmdline_utils/censor_cmdline.h"

#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace cmdline_utils {

static bool _isPasswordArgument(char const* argumentName);
static bool _isPasswordSwitch(char const* switchName);

static bool _isPasswordArgument(const char* argumentName) {
    static const char* const passwordArguments[] = {
        "net.ssl.PEMKeyPassword",
        "net.ssl.clusterPassword",
        "processManagement.windowsService.servicePassword",
        "security.kmip.clientCertificatePassword",
        NULL  // Last entry sentinel.
    };
    for (const char* const* current = passwordArguments; *current; ++current) {
        if (mongoutils::str::equals(argumentName, *current))
            return true;
    }
    return false;
}

static bool _isPasswordSwitch(const char* switchName) {
    static const char* const passwordSwitches[] = {
        "sslPEMKeyPassword",
        "sslClusterPassword",
        "servicePassword",
        "kmipClientCertificatePassword",
        NULL  // Last entry sentinel.
    };

    if (switchName[0] != '-')
        return false;
    size_t i = 1;
    if (switchName[1] == '-')
        i = 2;
    switchName += i;

    for (const char* const* current = passwordSwitches; *current; ++current) {
        if (mongoutils::str::equals(switchName, *current))
            return true;
    }
    return false;
}

static void _redact(char* arg) {
    for (; *arg; ++arg)
        *arg = 'x';
}

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
    BSONObjBuilder builder;
    censorBSONObjRecursive(*params, "", false, &builder);
    *params = builder.obj();
}

void censorArgsVector(std::vector<std::string>* args) {
    for (size_t i = 0; i < args->size(); ++i) {
        std::string& arg = args->at(i);
        const std::string::iterator endSwitch = std::find(arg.begin(), arg.end(), '=');
        std::string switchName(arg.begin(), endSwitch);
        if (_isPasswordSwitch(switchName.c_str())) {
            if (endSwitch == arg.end()) {
                if (i + 1 < args->size()) {
                    args->at(i + 1) = "<password>";
                }
            } else {
                arg = switchName + "=<password>";
            }
        }
    }
}

void censorArgvArray(int argc, char** argv) {
    // Algorithm:  For each arg in argv:
    //   Look for an equal sign in arg; if there is one, temporarily nul it out.
    //   check to see if arg is a password switch.  If so, overwrite the value
    //     component with xs.
    //   restore the nul'd out equal sign, if any.
    for (int i = 0; i < argc; ++i) {
        char* const arg = argv[i];
        char* const firstEqSign = strchr(arg, '=');
        if (NULL != firstEqSign) {
            *firstEqSign = '\0';
        }

        if (_isPasswordSwitch(arg)) {
            if (NULL == firstEqSign) {
                if (i + 1 < argc) {
                    _redact(argv[i + 1]);
                }
            } else {
                _redact(firstEqSign + 1);
            }
        }

        if (NULL != firstEqSign) {
            *firstEqSign = '=';
        }
    }
}
}  // namespace cmdline_utils
}
