// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/search/mongotmock/mongotmock_state.h"

namespace mongo {

namespace mongotmock {

namespace {
const auto getMongotMockStateDecoration = ServiceContext::declareDecoration<MongotMockState>();
}

/**
 * Fields that will only be validated if explicitly mocked as part of the expected command. If not
 * mocked, we allow these fields to be absent or present with any value. If a non-top level field
 * name needs to be ignored, include the full path and all intermediate paths such as
 * "parent.childA.childB", "childA.childB" and "childB".
 **/
const std::set<std::string> ignoredFields = {"lsid",
                                             "uid",
                                             "$db",
                                             "$traceCtx",
                                             "comment",
                                             "cursorOptions.docsRequested",
                                             "docsRequested",
                                             "cursorOptions.batchSize",
                                             "batchSize"};

// Checks that fieldName + "." is a prefix of an ignored field.
bool isParentPathOfAnIgnoredField(std::string fieldName) {

    for (const auto& ignoredField : ignoredFields) {
        if ((ignoredField.compare(0, fieldName.length() + 1, fieldName + ".") == 0)) {
            return true;
        }
    }
    return false;
}

MongotMockStateGuard getMongotMockState(ServiceContext* svc) {
    auto& mockState = getMongotMockStateDecoration(svc);
    return MongotMockStateGuard(&mockState);
}

bool checkGivenCommandMatchesExpectedCommand(const BSONObj& givenCmd, const BSONObj& expectedCmd) {
    // Recursively checks that the given command matches the expected command's values. The given
    // command must include all expected fields.
    for (auto&& expectedElem : expectedCmd) {
        auto&& givenElem = givenCmd[expectedElem.fieldNameStringData()];
        if (expectedElem.isABSONObj()) {
            if (!givenElem.isABSONObj() ||
                !checkGivenCommandMatchesExpectedCommand(givenElem.Obj(), expectedElem.Obj())) {
                return false;
            }
        } else if (!SimpleBSONElementComparator::kInstance.evaluate(givenElem == expectedElem)) {
            return false;
        }
    }

    // Checks that the given command doesn't have any additional fields compared to the expected
    // command, with the exception of fields explicitly listed in 'ignoredFields'. Any ignored
    // fields that are mocked in the expected command have been checked above.
    auto givenCommandFieldNames = givenCmd.getFieldNames<std::set<std::string>>();
    auto expectedCommandFieldNames = expectedCmd.getFieldNames<std::set<std::string>>();

    for (const auto& givenFieldName : givenCommandFieldNames) {
        if (!(ignoredFields.contains(givenFieldName) ||
              expectedCommandFieldNames.contains(givenFieldName))) {
            // If the fieldname is a prefix of an ignored field, we recursively check that its child
            // fields should also be ignored.
            auto&& givenElem = givenCmd[givenFieldName];
            if (givenElem.isABSONObj()) {
                if (isParentPathOfAnIgnoredField(givenFieldName) &&
                    checkGivenCommandMatchesExpectedCommand(givenElem.Obj(), BSONObj())) {
                    continue;
                }
            }
            return false;
        }
    }
    return true;
}

}  // namespace mongotmock
}  // namespace mongo
