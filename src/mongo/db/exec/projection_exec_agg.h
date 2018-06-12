/**
 * Copyright (C) 2018 MongoDB Inc.
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

#include <memory>

#include "mongo/bson/bsonobj.h"

namespace mongo {

/**
 * This class provides the query system with the ability to perform projections using the
 * aggregation system's projection semantics.
 */
class ProjectionExecAgg {
public:
    enum class ProjectionType { kInclusionProjection, kExclusionProjection };

    static std::unique_ptr<ProjectionExecAgg> create(BSONObj projSpec);

    ~ProjectionExecAgg();

    ProjectionType getType() const;

    BSONObj getProjectionSpec() const {
        return _projSpec;
    }

    BSONObj applyProjection(BSONObj inputDoc) const;

private:
    /**
     * ProjectionExecAgg::ProjectionExecutor wraps all agg-specific calls, and is forward-declared
     * here to avoid exposing any types from ParsedAggregationProjection to the query system.
     */
    class ProjectionExecutor;

    ProjectionExecAgg(BSONObj projSpec, std::unique_ptr<ProjectionExecutor> exec);

    std::unique_ptr<ProjectionExecutor> _exec;
    const BSONObj _projSpec;
};
}  // namespace mongo
