/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/plan_executor.h"

namespace mongo {

class DeleteRequest {
    MONGO_DISALLOW_COPYING(DeleteRequest);

public:
    explicit DeleteRequest(const NamespaceString& nsString)
        : _nsString(nsString),
          _multi(false),
          _god(false),
          _fromMigrate(false),
          _isExplain(false),
          _returnDeleted(false),
          _yieldPolicy(PlanExecutor::YIELD_MANUAL) {}

    void setQuery(const BSONObj& query) {
        _query = query;
    }
    void setProj(const BSONObj& proj) {
        _proj = proj;
    }
    void setSort(const BSONObj& sort) {
        _sort = sort;
    }
    void setCollation(const BSONObj& collation) {
        _collation = collation;
    }
    void setMulti(bool multi = true) {
        _multi = multi;
    }
    void setGod(bool god = true) {
        _god = god;
    }
    void setFromMigrate(bool fromMigrate = true) {
        _fromMigrate = fromMigrate;
    }
    void setExplain(bool isExplain = true) {
        _isExplain = isExplain;
    }
    void setReturnDeleted(bool returnDeleted = true) {
        _returnDeleted = returnDeleted;
    }
    void setYieldPolicy(PlanExecutor::YieldPolicy yieldPolicy) {
        _yieldPolicy = yieldPolicy;
    }

    const NamespaceString& getNamespaceString() const {
        return _nsString;
    }
    const BSONObj& getQuery() const {
        return _query;
    }
    const BSONObj& getProj() const {
        return _proj;
    }
    const BSONObj& getSort() const {
        return _sort;
    }
    const BSONObj& getCollation() const {
        return _collation;
    }
    bool isMulti() const {
        return _multi;
    }
    bool isGod() const {
        return _god;
    }
    bool isFromMigrate() const {
        return _fromMigrate;
    }
    bool isExplain() const {
        return _isExplain;
    }
    bool shouldReturnDeleted() const {
        return _returnDeleted;
    }
    PlanExecutor::YieldPolicy getYieldPolicy() const {
        return _yieldPolicy;
    }

private:
    const NamespaceString& _nsString;
    BSONObj _query;
    BSONObj _proj;
    BSONObj _sort;
    BSONObj _collation;
    bool _multi;
    bool _god;
    bool _fromMigrate;
    bool _isExplain;
    bool _returnDeleted;
    PlanExecutor::YieldPolicy _yieldPolicy;
};

}  // namespace mongo
