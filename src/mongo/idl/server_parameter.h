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

#pragma once
/* The contents of this file are meant to be used by
 * code generated from idlc.py.
 *
 * It should not be instantiated directly from mongo code,
 * rather parameters should be defined in .idl files.
 */

#include <functional>
#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_parameters.h"

namespace mongo {

/**
 * Specialization of ServerParameter used by IDL generator.
 */
class IDLServerParameter : public ServerParameter {
public:
    IDLServerParameter(StringData name, ServerParameterType paramType);

    /**
     * Define a callback for populating a BSONObj with the current setting.
     */
    using appendBSON_t = void(OperationContext*, BSONObjBuilder*, StringData);
    void setAppendBSON(std::function<appendBSON_t> appendBSON) {
        _appendBSON = std::move(appendBSON);
    }

    /**
     * Encode the setting into BSON object.
     *
     * Typically invoked by {getParameter:...} to produce a dictionary
     * of SCP settings.
     */
    void append(OperationContext* opCtx, BSONObjBuilder& b, const std::string& name) final;

    /**
     * Define a callback for setting the value from a BSONElement.
     */
    using fromBSON_t = Status(const BSONElement&);
    void setFromBSON(std::function<fromBSON_t> fromBSON) {
        _fromBSON = std::move(fromBSON);
    }

    /**
     * Update the underlying value using a BSONElement.
     *
     * Allows setting non-basic values (e.g. vector<string>)
     * via the {setParameter: ...} call.
     */
    Status set(const BSONElement& newValueElement) final;

    /**
     * Define a callback for setting the value from a string.
     */
    using fromString_t = Status(StringData);
    void setFromString(std::function<fromString_t> fromString) {
        _fromString = std::move(fromString);
    }

    /**
     * Update the underlying value from a string.
     *
     * Typically invoked from commandline --setParameter usage.
     */
    Status setFromString(const std::string& str) final;

protected:
    std::function<appendBSON_t> _appendBSON;
    std::function<fromBSON_t> _fromBSON;
    std::function<fromString_t> _fromString;
};

/**
 * Proxy instance for deprecated aliases of set parameters.
 */
class IDLServerParameterDeprecatedAlias : public ServerParameter {
public:
    IDLServerParameterDeprecatedAlias(StringData name, ServerParameter* sp);

    void append(OperationContext* opCtx, BSONObjBuilder& b, const std::string& name) final;
    Status set(const BSONElement& newValueElement) final;
    Status setFromString(const std::string& str) final;

private:
    ServerParameter* _sp;
};

}  // namespace mongo
