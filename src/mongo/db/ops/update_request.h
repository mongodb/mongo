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

#include "mongo/db/curop.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/write_ops_gen.h"
#include "mongo/db/ops/write_ops_parsers.h"
#include "mongo/db/pipeline/runtime_constants_gen.h"
#include "mongo/db/query/explain.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {
const std::vector<BSONObj> emptyArrayFilters{};
const BSONObj emptyCollation{};
}  // namespace

class FieldRef;

class UpdateRequest {
public:
    enum ReturnDocOption {
        // Return no document.
        RETURN_NONE,

        // Return the document as it was before the update. If the update results in an insert,
        // no document will be returned.
        RETURN_OLD,

        // Return the document as it is after the update.
        RETURN_NEW
    };

    UpdateRequest(const write_ops::UpdateOpEntry& updateOp = write_ops::UpdateOpEntry())
        : _updateOp(updateOp) {}

    void setNamespaceString(const NamespaceString& nsString) {
        _nsString = nsString;
    }

    const NamespaceString& getNamespaceString() const {
        return _nsString;
    }

    void setQuery(const BSONObj& query) {
        _updateOp.setQ(query);
    }

    const BSONObj& getQuery() const {
        return _updateOp.getQ();
    }

    void setProj(const BSONObj& proj) {
        _proj = proj;
    }

    const BSONObj& getProj() const {
        return _proj;
    }

    void setSort(const BSONObj& sort) {
        _sort = sort;
    }

    const BSONObj& getSort() const {
        return _sort;
    }

    void setCollation(const BSONObj& collation) {
        _updateOp.setCollation(collation);
    }

    const BSONObj& getCollation() const {
        return _updateOp.getCollation().get_value_or(emptyCollation);
    }

    void setUpdateModification(const write_ops::UpdateModification& updateMod) {
        _updateOp.setU(updateMod);
    }

    const write_ops::UpdateModification& getUpdateModification() const {
        return _updateOp.getU();
    }

    void setUpdateConstants(const boost::optional<BSONObj>& updateConstants) {
        _updateOp.setC(updateConstants);
    }

    const boost::optional<BSONObj>& getUpdateConstants() const {
        return _updateOp.getC();
    }

    void setRuntimeConstants(RuntimeConstants runtimeConstants) {
        _runtimeConstants = std::move(runtimeConstants);
    }

    const boost::optional<RuntimeConstants>& getRuntimeConstants() const {
        return _runtimeConstants;
    }

    void setArrayFilters(const std::vector<BSONObj>& arrayFilters) {
        _updateOp.setArrayFilters(arrayFilters);
    }

    const std::vector<BSONObj>& getArrayFilters() const {
        return _updateOp.getArrayFilters().get_value_or(emptyArrayFilters);
    }

    // Please see documentation on the private members matching these names for
    // explanations of the following fields.

    void setGod(bool value = true) {
        _god = value;
    }

    bool isGod() const {
        return _god;
    }

    void setUpsert(bool value = true) {
        _updateOp.setUpsert(value);
    }

    bool isUpsert() const {
        return _updateOp.getUpsert();
    }

    void setUpsertSuppliedDocument(bool value = true) {
        _updateOp.setUpsertSupplied(value);
    }

    bool shouldUpsertSuppliedDocument() const {
        return _updateOp.getUpsertSupplied();
    }

    void setMulti(bool value = true) {
        _updateOp.setMulti(value);
    }

    bool isMulti() const {
        return _updateOp.getMulti();
    }

    void setFromMigration(bool value = true) {
        _fromMigration = value;
    }

    bool isFromMigration() const {
        return _fromMigration;
    }

    void setFromOplogApplication(bool value = true) {
        _fromOplogApplication = value;
    }

    bool isFromOplogApplication() const {
        return _fromOplogApplication;
    }

    void setExplain(bool value = true) {
        _isExplain = value;
    }

    bool isExplain() const {
        return _isExplain;
    }

    void setReturnDocs(ReturnDocOption value) {
        _returnDocs = value;
    }

    void setHint(const BSONObj& hint) {
        _updateOp.setHint(hint);
    }

    BSONObj getHint() const {
        return _updateOp.getHint();
    }

    bool shouldReturnOldDocs() const {
        return _returnDocs == ReturnDocOption::RETURN_OLD;
    }

    bool shouldReturnNewDocs() const {
        return _returnDocs == ReturnDocOption::RETURN_NEW;
    }

    bool shouldReturnAnyDocs() const {
        return shouldReturnOldDocs() || shouldReturnNewDocs();
    }

    void setYieldPolicy(PlanExecutor::YieldPolicy yieldPolicy) {
        _yieldPolicy = yieldPolicy;
    }

    PlanExecutor::YieldPolicy getYieldPolicy() const {
        return _yieldPolicy;
    }

    void setStmtId(StmtId stmtId) {
        _stmtId = std::move(stmtId);
    }

    StmtId getStmtId() const {
        return _stmtId;
    }

    const std::string toString() const {
        StringBuilder builder;
        builder << " query: " << getQuery();
        builder << " projection: " << _proj;
        builder << " sort: " << _sort;
        builder << " collation: " << getCollation();
        builder << " updateModification: " << getUpdateModification().toString();
        builder << " stmtId: " << _stmtId;

        builder << " arrayFilters: [";
        bool first = true;
        for (auto arrayFilter : getArrayFilters()) {
            if (!first) {
                builder << ", ";
            }
            first = false;
            builder << arrayFilter;
        }
        builder << "]";

        if (getUpdateConstants()) {
            builder << " updateConstants: " << *getUpdateConstants();
        }

        if (_runtimeConstants) {
            builder << " runtimeConstants: " << _runtimeConstants->toBSON().toString();
        }

        builder << " god: " << _god;
        builder << " upsert: " << isUpsert();
        builder << " multi: " << isMulti();
        builder << " fromMigration: " << _fromMigration;
        builder << " fromOplogApplication: " << _fromOplogApplication;
        builder << " isExplain: " << _isExplain;
        return builder.str();
    }

private:
    NamespaceString _nsString;

    write_ops::UpdateOpEntry _updateOp;

    // Contains the projection information.
    BSONObj _proj;

    // Contains the sort order information.
    BSONObj _sort;

    // System-defined constant values which may be required by the query or update operation.
    boost::optional<RuntimeConstants> _runtimeConstants;


    // The statement id of this request.
    StmtId _stmtId = kUninitializedStmtId;

    // Flags controlling the update.

    // God bypasses _id checking and index generation. It is only used on behalf of system
    // updates, never user updates.
    bool _god = false;

    // True if this update is on behalf of a chunk migration.
    bool _fromMigration = false;

    // True if this update was triggered by the application of an oplog entry.
    bool _fromOplogApplication = false;

    // Whether or not we are requesting an explained update. Explained updates are read-only.
    bool _isExplain = false;

    // Specifies which version of the documents to return, if any.
    //
    //   RETURN_NONE (default): Never return any documents, old or new.
    //   RETURN_OLD: Return ADVANCED when a matching document is encountered, and the value of
    //               the document before it was updated. If there were no matches, return
    //               IS_EOF instead (even in case of an upsert).
    //   RETURN_NEW: Return ADVANCED when a matching document is encountered, and the value of
    //               the document after being updated. If an upsert was specified and it
    //               resulted in an insert, return the inserted document.
    //
    // This allows findAndModify to execute an update and retrieve the resulting document
    // without another query before or after the update.
    ReturnDocOption _returnDocs = ReturnDocOption::RETURN_NONE;

    // Whether or not the update should yield. Defaults to NO_YIELD.
    PlanExecutor::YieldPolicy _yieldPolicy = PlanExecutor::NO_YIELD;
};

}  // namespace mongo
