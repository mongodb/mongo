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

#include "mongo/db/concurrency/locker_noop_service_context_test_fixture.h"
#include "mongo/db/service_context.h"
#include "mongo/db/update/update_node.h"
#include "mongo/db/update/v2_log_builder.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class UpdateTestFixture : public LockerNoopServiceContextTest {
public:
    ~UpdateTestFixture() override = default;

protected:
    // Creates a RuntimeUpdatePath from a string, assuming that all numeric path components are
    // array indexes. Tests which use numeric field names in objects must manually create a
    // RuntimeUpdatePath.
    static RuntimeUpdatePath makeRuntimeUpdatePathForTest(StringData path) {
        FieldRef fr(path);
        std::vector<RuntimeUpdatePath::ComponentType> types;

        for (int i = 0; i < fr.numParts(); ++i) {
            types.push_back(fr.isNumericPathComponentStrict(i)
                                ? RuntimeUpdatePath::ComponentType::kArrayIndex
                                : RuntimeUpdatePath::ComponentType::kFieldName);
        }

        return RuntimeUpdatePath(fr, std::move(types));
    }

    void setUp() override {
        resetApplyParams();
    }

    virtual void resetApplyParams() {
        _immutablePathsVector.clear();
        _immutablePaths.clear();
        _pathToCreate = std::make_shared<FieldRef>();
        _pathTaken = std::make_shared<RuntimeUpdatePath>();
        _matchedField = StringData();
        _insert = false;
        _fromOplogApplication = false;
        _validateForStorage = true;
        _indexData.reset();
        _logDoc.reset();
        _logBuilder = std::make_unique<v2_log_builder::V2LogBuilder>();
        _modifiedPaths.clear();
    }

    virtual UpdateExecutor::ApplyParams getApplyParams(mutablebson::Element element) {
        UpdateExecutor::ApplyParams applyParams(element, _immutablePaths);
        applyParams.matchedField = _matchedField;
        applyParams.insert = _insert;
        applyParams.fromOplogApplication = _fromOplogApplication;
        applyParams.validateForStorage = _validateForStorage;
        applyParams.indexData = _indexData.get();
        applyParams.modifiedPaths = &_modifiedPaths;
        applyParams.logMode = ApplyParams::LogMode::kGenerateOplogEntry;
        return applyParams;
    }

    UpdateNode::UpdateNodeApplyParams getUpdateNodeApplyParams() {
        UpdateNode::UpdateNodeApplyParams applyParams;
        applyParams.pathToCreate = _pathToCreate;
        applyParams.pathTaken = _pathTaken;
        applyParams.logBuilder = _logBuilder.get();
        return applyParams;
    }

    void addImmutablePath(StringData path) {
        auto fieldRef = std::make_unique<FieldRef>(path);
        _immutablePathsVector.push_back(std::move(fieldRef));
        _immutablePaths.insert(_immutablePathsVector.back().get());
    }

    void setPathToCreate(StringData path) {
        _pathToCreate->clear();
        _pathToCreate->parse(path);
    }

    void setPathTaken(const RuntimeUpdatePath& pathTaken) {
        *_pathTaken = pathTaken;
    }

    void setMatchedField(StringData matchedField) {
        _matchedField = matchedField;
    }

    void setInsert(bool insert) {
        _insert = insert;
    }

    void setFromOplogApplication(bool fromOplogApplication) {
        _fromOplogApplication = fromOplogApplication;
    }

    void setValidateForStorage(bool validateForStorage) {
        _validateForStorage = validateForStorage;
    }

    void addIndexedPath(StringData path) {
        if (!_indexData) {
            _indexData = std::make_unique<UpdateIndexData>();
        }
        _indexData->addPath(FieldRef(path));
    }

    void setLogBuilderToNull() {
        _logBuilder.reset();
    }

    std::string getModifiedPaths() {
        return _modifiedPaths.toString();
    }

    BSONObj getOplogEntry() const {
        return _logBuilder->serialize();
    }

    void assertOplogEntryIsNoop() const {
        ASSERT_BSONOBJ_BINARY_EQ(getOplogEntry(), fromjson("{$v:2, diff: {}}"));
    }

    void assertOplogEntry(const BSONObj& expectedV2Entry, bool checkBinaryEquality = true) {
        if (checkBinaryEquality) {
            ASSERT_BSONOBJ_BINARY_EQ(expectedV2Entry, getOplogEntry());
        } else {
            ASSERT_BSONOBJ_EQ(expectedV2Entry, getOplogEntry());
        }
    }

private:
    std::vector<std::unique_ptr<FieldRef>> _immutablePathsVector;
    FieldRefSet _immutablePaths;
    std::shared_ptr<FieldRef> _pathToCreate;
    std::shared_ptr<RuntimeUpdatePath> _pathTaken;
    StringData _matchedField;
    bool _insert;
    bool _fromOplogApplication;
    bool _validateForStorage;
    std::unique_ptr<UpdateIndexData> _indexData;
    mutablebson::Document _logDoc;
    std::unique_ptr<LogBuilderInterface> _logBuilder;
    FieldRefSetWithStorage _modifiedPaths;
};

}  // namespace mongo
