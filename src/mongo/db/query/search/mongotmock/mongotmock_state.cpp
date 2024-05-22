/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/search/mongotmock/mongotmock_state.h"

namespace mongo {

namespace mongotmock {

namespace {
const auto getMongotMockStateDecoration = ServiceContext::declareDecoration<MongotMockState>();
}

MongotMockStateGuard getMongotMockState(ServiceContext* svc) {
    auto& mockState = getMongotMockStateDecoration(svc);
    return MongotMockStateGuard(&mockState);
}

bool checkUserCommandMatchesExpectedCommand(const BSONObj& userCmd, const BSONObj& expectedCmd) {
    // Recursively checks that the given command matches the expected command's values. The given
    // command must include all expected fields, but may have superfluous (unexpected) fields.
    for (auto&& expectedElem : expectedCmd) {
        auto&& userElem = userCmd[expectedElem.fieldNameStringData()];
        if (expectedElem.isABSONObj()) {
            if (!userElem.isABSONObj() ||
                !checkUserCommandMatchesExpectedCommand(userElem.Obj(), expectedElem.Obj())) {
                return false;
            }
        } else if (!SimpleBSONElementComparator::kInstance.evaluate(userElem == expectedElem)) {
            return false;
        }
    }
    return true;
}

}  // namespace mongotmock
}  // namespace mongo
