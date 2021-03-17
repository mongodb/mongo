/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/bsontypes.h"
#include "mongo/idl/feature_flag.h"
#include "mongo/idl/server_parameter.h"

namespace mongo {

/**
 * Test-only RAII type that allows to set a server parameter value during the execution of a
 * unit test, or part of a unit test, and resets it to the original value on destruction.
 */
class RAIIServerParameterControllerForTest {
public:
    /**
     * Constructor setting the server parameter to the specified value.
     */
    template <typename T>
    RAIIServerParameterControllerForTest(const std::string& paramName, T value)
        : _serverParam(_getServerParameter(paramName)) {
        // Save the old value
        BSONObjBuilder bob;
        _serverParam->appendSupportingRoundtrip(nullptr, bob, paramName);
        _oldValue = bob.obj();

        // Set to the new value
        uassertStatusOK(_serverParam->set(BSON(paramName << value).firstElement()));
    }

    /**
     * Destructor resetting the server parameter to the original value.
     */
    ~RAIIServerParameterControllerForTest() {
        // Reset to the old value
        auto elem = _oldValue.firstElement();
        uassertStatusOK(_serverParam->set(elem));
    }

private:
    /**
     * Returns a server parameter if exists, otherwise triggers an invariant.
     */
    ServerParameter* _getServerParameter(const std::string& paramName) {
        const auto& spMap = ServerParameterSet::getGlobal()->getMap();
        const auto& spIt = spMap.find(paramName);
        invariant(spIt != spMap.end());

        auto* sp = spIt->second;
        invariant(sp);
        return sp;
    }

    ServerParameter* _serverParam;

    BSONObj _oldValue;
};

}  // namespace mongo
