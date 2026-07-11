// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string>

namespace mongo {
/**
 * This is a wrapper class encapsulating common logic for executing JavaScript code in $where match
 * expression which can be used both in the SBE and classic engines.
 */
class JsFunction {
public:
    JsFunction(OperationContext* opCtx, std::string code, const DatabaseName& dbName);
    JsFunction(const JsFunction& other);
    JsFunction(JsFunction&& other) = delete;

    JsFunction& operator=(const JsFunction& other);
    JsFunction& operator=(JsFunction&& other) = delete;

    bool runAsPredicate(const BSONObj& obj) const;

    const std::string& getCode() const {
        return _code;
    }

    const DatabaseName& getDbName() const {
        return _dbName;
    }

    size_t getApproximateSize() const;

private:
    void _init(OperationContext* opCtx, std::string code, const DatabaseName& dbName);

    std::string _code;
    DatabaseName _dbName;
    std::unique_ptr<Scope> _scope;
    ScriptingFunction _func;
};

}  // namespace mongo
