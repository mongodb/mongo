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
#pragma once

#include <string>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_parameter.h"

namespace mongo {

/**
 * Test-only class that sets a server parameter to the specified value and allows
 * resetting after the test completes.
 */
class ServerParameterControllerForTest {
public:
    /**
     * Constructor setting the server parameter to the specified value.
     */
    template <typename T>
    ServerParameterControllerForTest(const std::string& name, T value)
        : _serverParam(ServerParameterSet::getNodeParameterSet()->get(name)) {
        // Save the old value.
        BSONObjBuilder bob;
        _serverParam->appendSupportingRoundtrip(nullptr, &bob, name, boost::none);
        _oldValue = bob.obj();

        // Set server param to the new value.
        uassertStatusOK(_serverParam->set(BSON(name << value).firstElement(), boost::none));
    }

    void reset() {
        // Reset to the old value.
        auto elem = _oldValue.firstElement();
        uassertStatusOK(_serverParam->set(elem, boost::none));
    }

private:
    ServerParameter* _serverParam;
    BSONObj _oldValue;
};

/**
 * Test-only RAII type that wraps ServerParameterControllerForTest. Upon destruction, the server
 * parameter will be set to its original value.
 */
class RAIIServerParameterControllerForTest {
public:
    /**
     * Constructor setting the server parameter to the specified value.
     */
    template <typename T>
    RAIIServerParameterControllerForTest(const std::string& name, T value)
        : _serverParamController(ServerParameterControllerForTest(name, value)) {}

    /**
     * Destructor resetting the server parameter to the original value.
     */
    ~RAIIServerParameterControllerForTest() {
        _serverParamController.reset();
    }

private:
    ServerParameterControllerForTest _serverParamController;
};

}  // namespace mongo
