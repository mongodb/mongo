// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/db/update/update_node.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/update/v2_log_builder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

class UpdateTestFixture : public ServiceContextTest {
public:
    ~UpdateTestFixture() override = default;

protected:
    // Creates a RuntimeUpdatePath from a string, assuming that all numeric path components are
    // array indexes. Tests which use numeric field names in objects must manually create a
    // RuntimeUpdatePath.
    static RuntimeUpdatePath makeRuntimeUpdatePathForTest(std::string_view path) {
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
        _matchedField = std::string_view();
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
        applyParams.modifiedPaths = &_modifiedPaths;
        applyParams.logMode = ApplyParams::LogMode::kGenerateOplogEntry;
        return applyParams;
    }

    bool getIndexAffectedFromLogEntry(BSONObj logEntry) {
        if (!_indexData) {
            return false;
        }
        auto diffElem = logEntry[update_oplog_entry::kDiffObjectFieldName];
        if (diffElem.type() != BSONType::object) {
            return false;
        }

        mongo::doc_diff::IndexUpdateIdentifier updateIdentifier{1 /*numIndexes*/};
        updateIdentifier.addIndex(0 /*indexCounter*/, *_indexData);
        return updateIdentifier.determineAffectedIndexes(diffElem.embeddedObject()).any();
    }

    bool getIndexAffectedFromLogEntry() {
        if (!_logBuilder) {
            return false;
        }
        return getIndexAffectedFromLogEntry(getOplogEntry());
    }

    UpdateNode::UpdateNodeApplyParams getUpdateNodeApplyParams() {
        UpdateNode::UpdateNodeApplyParams applyParams;
        applyParams.pathToCreate = _pathToCreate;
        applyParams.pathTaken = _pathTaken;
        applyParams.logBuilder = _logBuilder.get();
        return applyParams;
    }

    void addImmutablePath(std::string_view path) {
        auto fieldRef = std::make_unique<FieldRef>(path);
        _immutablePathsVector.push_back(std::move(fieldRef));
        _immutablePaths.insert(_immutablePathsVector.back().get());
    }

    void setPathToCreate(std::string_view path) {
        _pathToCreate->clear();
        _pathToCreate->parse(path);
    }

    void setPathTaken(const RuntimeUpdatePath& pathTaken) {
        *_pathTaken = pathTaken;
    }

    void setMatchedField(std::string_view matchedField) {
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

    void addIndexedPath(std::string_view path) {
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
    std::string_view _matchedField;
    bool _insert;
    bool _fromOplogApplication;
    bool _validateForStorage;
    std::unique_ptr<UpdateIndexData> _indexData;
    mutablebson::Document _logDoc;
    std::unique_ptr<LogBuilderInterface> _logBuilder;
    FieldRefSetWithStorage _modifiedPaths;
};

}  // namespace mongo
