/**
 *    Copyright (C) 2016 MongoDB, Inc.
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

#include <boost/intrusive_ptr.hpp>
#include <memory>

namespace mongo {

class BSONObj;
class Document;
struct DepsTracker;
struct ExpressionContext;

namespace parsed_aggregation_projection {

enum class ProjectionType { kExclusion, kInclusion };

/**
 * A ParsedAggregationProjection is responsible for parsing and executing a projection. It
 * represents either an inclusion or exclusion projection. This is the common interface between the
 * two types of projections.
 */
class ParsedAggregationProjection {
public:
    /**
     * Main entry point for a ParsedAggregationProjection.
     *
     * Throws a UserException if 'spec' is an invalid projection specification.
     */
    static std::unique_ptr<ParsedAggregationProjection> create(const BSONObj& spec);

    virtual ~ParsedAggregationProjection() = default;

    /**
     * Returns the type of projection represented by this ParsedAggregationProjection.
     */
    virtual ProjectionType getType() const = 0;

    /**
     * Parse the user-specified BSON object 'spec'. By the time this is called, 'spec' has already
     * been verified to not have any conflicting path specifications, and not to mix and match
     * inclusions and exclusions. 'variablesParseState' is used by any contained expressions to
     * track which variables are defined so that they can later be referenced at execution time.
     */
    virtual void parse(const BSONObj& spec) = 0;

    /**
     * Serialize this projection.
     */
    virtual Document serialize(bool explain = false) const = 0;

    /**
     * Optimize any expressions contained within this projection.
     */
    virtual void optimize() {}

    /**
     * Inject the ExpressionContext into any expressions contained within this projection.
     */
    virtual void injectExpressionContext(const boost::intrusive_ptr<ExpressionContext>& expCtx) {}

    /**
     * Add any dependencies needed by this projection or any sub-expressions to 'deps'.
     */
    virtual void addDependencies(DepsTracker* deps) const {}

    /**
     * Apply the projection to 'input'.
     */
    virtual Document applyProjection(Document input) const = 0;

protected:
    ParsedAggregationProjection() = default;
};
}  // namespace parsed_aggregation_projection
}  // namespace mongo
