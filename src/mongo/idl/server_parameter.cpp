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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

#include "mongo/idl/server_parameter.h"

#include "mongo/logv2/log.h"

namespace mongo {
using SPT = ServerParameterType;

MONGO_INITIALIZER_GROUP(BeginServerParameterRegistration, (), ("EndServerParameterRegistration"))
MONGO_INITIALIZER_GROUP(EndServerParameterRegistration,
                        ("BeginServerParameterRegistration"),
                        ("BeginStartupOptionHandling"))

ServerParameter::ServerParameter(StringData name, ServerParameterType spt, NoRegistrationTag)
    : _name(name.toString()), _type(spt) {
    _generation.clear();
}

ServerParameter::ServerParameter(StringData name, ServerParameterType spt)
    : ServerParameter(name, spt, NoRegistrationTag{}) {
    ServerParameterSet::getParameterSet(spt)->add(this);
}

void ServerParameter::setGeneration(const OID& generation) {
    uassert(6225101,
            "Invalid call to setGeneration on locally scoped server parameter",
            isClusterWide());
    _generation = generation;
}

namespace {
class NodeParameterSet : public ServerParameterSet {
public:
    void add(ServerParameter* sp) final {
        uassert(6225102,
                str::stream() << "Registering cluster-wide parameter '" << sp->name()
                              << "' as node-local server parameter",
                sp->isNodeLocal());
        ServerParameter*& x = _map[sp->name()];
        uassert(23784,
                str::stream() << "Duplicate server parameter registration for '" << x->name()
                              << "'",
                !x);
        x = sp;
    }
};
NodeParameterSet* gNodeServerParameters = nullptr;

class ClusterParameterSet : public ServerParameterSet {
public:
    void add(ServerParameter* sp) final {
        uassert(6225103,
                str::stream() << "Registering node-local parameter '" << sp->name()
                              << "' as cluster-wide server parameter",
                sp->isClusterWide());
        ServerParameter*& x = _map[sp->name()];
        uassert(6225104,
                str::stream() << "Duplicate cluster-wide server parameter registration for '"
                              << x->name() << "'",
                !x);
        x = sp;
    }
};
ClusterParameterSet* gClusterServerParameters;
}  // namespace

ServerParameterSet* ServerParameterSet::getNodeParameterSet() {
    if (!gNodeServerParameters) {
        gNodeServerParameters = new NodeParameterSet();
    }
    return gNodeServerParameters;
}

ServerParameterSet* ServerParameterSet::getClusterParameterSet() {
    if (!gClusterServerParameters) {
        gClusterServerParameters = new ClusterParameterSet();
    }
    return gClusterServerParameters;
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

void ServerParameterSet::remove(const std::string& name) {
    invariant(1 == _map.erase(name));
}

IDLServerParameterDeprecatedAlias::IDLServerParameterDeprecatedAlias(StringData name,
                                                                     ServerParameter* sp)
    : ServerParameter(name, sp->getServerParameterType()), _sp(sp) {
    if (_sp->isTestOnly()) {
        setTestOnly();
    }
}

void IDLServerParameterDeprecatedAlias::append(OperationContext* opCtx,
                                               BSONObjBuilder& b,
                                               const std::string& fieldName) {
    std::call_once(_warnOnce, [&] {
        LOGV2_WARNING(23781,
                      "Use of deprecated server parameter '{deprecatedName}', "
                      "please use '{canonicalName}' instead",
                      "Use of deprecated server parameter name",
                      "deprecatedName"_attr = name(),
                      "canonicalName"_attr = _sp->name());
    });
    _sp->append(opCtx, b, fieldName);
}

Status IDLServerParameterDeprecatedAlias::set(const BSONElement& newValueElement) {
    std::call_once(_warnOnce, [&] {
        LOGV2_WARNING(23782,
                      "Use of deprecated server parameter '{deprecatedName}', "
                      "please use '{canonicalName}' instead",
                      "Use of deprecated server parameter name",
                      "deprecatedName"_attr = name(),
                      "canonicalName"_attr = _sp->name());
    });
    return _sp->set(newValueElement);
}

Status IDLServerParameterDeprecatedAlias::setFromString(const std::string& str) {
    std::call_once(_warnOnce, [&] {
        LOGV2_WARNING(23783,
                      "Use of deprecated server parameter '{deprecatedName}', "
                      "please use '{canonicalName}' instead",
                      "Use of deprecated server parameter name",
                      "deprecatedName"_attr = name(),
                      "canonicalName"_attr = _sp->name());
    });
    return _sp->setFromString(str);
}

namespace {
class DisabledTestParameter : public ServerParameter {
public:
    DisabledTestParameter() = delete;

    DisabledTestParameter(ServerParameter* sp)
        : ServerParameter(sp->name(), sp->getServerParameterType(), NoRegistrationTag{}), _sp(sp) {
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
