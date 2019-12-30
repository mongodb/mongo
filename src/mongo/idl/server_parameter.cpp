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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/idl/server_parameter.h"

#include "mongo/util/log.h"

namespace mongo {
using SPT = ServerParameterType;

MONGO_INITIALIZER_GROUP(BeginServerParameterRegistration,
                        MONGO_NO_PREREQUISITES,
                        ("EndServerParameterRegistration"))
MONGO_INITIALIZER_GROUP(EndServerParameterRegistration,
                        ("BeginServerParameterRegistration"),
                        ("BeginStartupOptionHandling"))

ServerParameter::ServerParameter(StringData name, ServerParameterType spt)
    : ServerParameter(ServerParameterSet::getGlobal(),
                      name,
                      spt != SPT::kRuntimeOnly,
                      spt != SPT::kStartupOnly) {}

ServerParameter::ServerParameter(ServerParameterSet* sps,
                                 StringData name,
                                 bool allowedToChangeAtStartup,
                                 bool allowedToChangeAtRuntime)
    : _name(name.toString()),
      _allowedToChangeAtStartup(allowedToChangeAtStartup),
      _allowedToChangeAtRuntime(allowedToChangeAtRuntime) {
    if (sps) {
        sps->add(this);
    }
}

ServerParameter::ServerParameter(ServerParameterSet* sps, StringData name)
    : _name(name.toString()), _allowedToChangeAtStartup(true), _allowedToChangeAtRuntime(true) {
    if (sps) {
        sps->add(this);
    }
}

namespace {
ServerParameterSet* gGlobalServerParameterSet = nullptr;
}  // namespace

ServerParameterSet* ServerParameterSet::getGlobal() {
    if (!gGlobalServerParameterSet) {
        gGlobalServerParameterSet = new ServerParameterSet();
    }
    return gGlobalServerParameterSet;
}

void ServerParameterSet::add(ServerParameter* sp) {
    ServerParameter*& x = _map[sp->name()];
    if (x) {
        severe() << "'" << x->name() << "' already exists in the server parameter set.";
        abort();
    }
    x = sp;
}

StatusWith<std::string> ServerParameter::coerceToString(const BSONElement& element, bool redact) {
    switch (element.type()) {
        case NumberDouble:
            return std::to_string(element.Double());
        case String:
            return element.String();
        case NumberInt:
            return std::to_string(element.Int());
        case NumberLong:
            return std::to_string(element.Long());
        case Date:
            return dateToISOStringLocal(element.Date());
        default:
            std::string diag;
            if (redact) {
                diag = "###";
            } else {
                diag = element.toString();
            }
            return {ErrorCodes::BadValue,
                    str::stream() << "Unsupported type " << typeName(element.type()) << " (value: '"
                                  << diag << "') for setParameter: " << name()};
    }
}

IDLServerParameterDeprecatedAlias::IDLServerParameterDeprecatedAlias(StringData name,
                                                                     ServerParameter* sp)
    : ServerParameter(ServerParameterSet::getGlobal(),
                      name,
                      sp->allowedToChangeAtStartup(),
                      sp->allowedToChangeAtRuntime()),
      _sp(sp) {
    if (_sp->isTestOnly()) {
        setTestOnly();
    }
}

void IDLServerParameterDeprecatedAlias::append(OperationContext* opCtx,
                                               BSONObjBuilder& b,
                                               const std::string& fieldName) {
    std::call_once(_warnOnce, [&] {
        warning() << "Use of deprecated server parameter '" << name() << "', please use '"
                  << _sp->name() << "' instead.";
    });
    _sp->append(opCtx, b, fieldName);
}

Status IDLServerParameterDeprecatedAlias::set(const BSONElement& newValueElement) {
    std::call_once(_warnOnce, [&] {
        warning() << "Use of deprecated server parameter '" << name() << "', please use '"
                  << _sp->name() << "' instead.";
    });
    return _sp->set(newValueElement);
}

Status IDLServerParameterDeprecatedAlias::setFromString(const std::string& str) {
    std::call_once(_warnOnce, [&] {
        warning() << "Use of deprecated server parameter '" << name() << "', please use '"
                  << _sp->name() << "' instead.";
    });
    return _sp->setFromString(str);
}

namespace {
class DisabledTestParameter : public ServerParameter {
public:
    DisabledTestParameter() = delete;

    DisabledTestParameter(ServerParameter* sp)
        : ServerParameter(
              nullptr, sp->name(), sp->allowedToChangeAtStartup(), sp->allowedToChangeAtRuntime()),
          _sp(sp) {
        setTestOnly();
    }

    void append(OperationContext* opCtx, BSONObjBuilder& b, const std::string& name) final {}

    Status setFromString(const std::string&) final {
        return {ErrorCodes::BadValue,
                str::stream() << "setParameter: '" << name()
                              << "' is only supported with 'enableTestCommands=true'"};
    }

    Status set(const BSONElement& newValueElement) final {
        return setFromString("");
    }

private:
    // Retain the original pointer to avoid ASAN complaining.
    ServerParameter* _sp;
};
}  // namespace

void ServerParameterSet::disableTestParameters() {
    for (auto& spit : _map) {
        auto*& sp = spit.second;
        if (sp->isTestOnly()) {
            sp = new DisabledTestParameter(sp);
        }
    }
}

}  // namespace mongo
