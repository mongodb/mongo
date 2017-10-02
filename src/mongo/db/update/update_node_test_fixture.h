/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/db/logical_clock.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/db/update/update_node.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

class UpdateNodeTest : public mongo::unittest::Test {
public:
    ~UpdateNodeTest() override = default;

protected:
    void setUp() override {
        resetApplyParams();

        // Set up the logical clock needed by CurrentDateNode and ObjectReplaceNode.
        auto service = mongo::getGlobalServiceContext();
        auto logicalClock = mongo::stdx::make_unique<mongo::LogicalClock>(service);
        mongo::LogicalClock::set(service, std::move(logicalClock));
    }

    void resetApplyParams() {
        _immutablePathsVector.clear();
        _immutablePaths.clear();
        _pathToCreate = std::make_shared<FieldRef>();
        _pathTaken = std::make_shared<FieldRef>();
        _matchedField = StringData();
        _insert = false;
        _fromOplogApplication = false;
        _validateForStorage = true;
        _indexData.reset();
        _logDoc.reset();
        _logBuilder = stdx::make_unique<LogBuilder>(_logDoc.root());
    }

    UpdateNode::ApplyParams getApplyParams(mutablebson::Element element) {
        UpdateNode::ApplyParams applyParams(element, _immutablePaths);
        applyParams.pathToCreate = _pathToCreate;
        applyParams.pathTaken = _pathTaken;
        applyParams.matchedField = _matchedField;
        applyParams.insert = _insert;
        applyParams.fromOplogApplication = _fromOplogApplication;
        applyParams.validateForStorage = _validateForStorage;
        applyParams.indexData = _indexData.get();
        applyParams.logBuilder = _logBuilder.get();
        return applyParams;
    }

    void addImmutablePath(StringData path) {
        auto fieldRef = stdx::make_unique<FieldRef>(path);
        _immutablePathsVector.push_back(std::move(fieldRef));
        _immutablePaths.insert(_immutablePathsVector.back().get());
    }

    void setPathToCreate(StringData path) {
        _pathToCreate->clear();
        _pathToCreate->parse(path);
    }

    void setPathTaken(StringData path) {
        _pathTaken->clear();
        _pathTaken->parse(path);
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
            _indexData = stdx::make_unique<UpdateIndexData>();
        }
        _indexData->addPath(path);
    }

    void setLogBuilderToNull() {
        _logBuilder.reset();
    }

    const mutablebson::Document& getLogDoc() {
        return _logDoc;
    }

private:
    std::vector<std::unique_ptr<FieldRef>> _immutablePathsVector;
    FieldRefSet _immutablePaths;
    std::shared_ptr<FieldRef> _pathToCreate;
    std::shared_ptr<FieldRef> _pathTaken;
    StringData _matchedField;
    bool _insert;
    bool _fromOplogApplication;
    bool _validateForStorage;
    std::unique_ptr<UpdateIndexData> _indexData;
    mutablebson::Document _logDoc;
    std::unique_ptr<LogBuilder> _logBuilder;
};

}  // namespace mongo
