/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include <vector>

#include "mongo/base/init.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {

/**
 * Registers a GranularityRounder with the name 'name' and initializer key 'key'. When a
 * granularity specification with name 'name' is found, 'rounder' will be called to construct a
 * GranularityRounder for that granularity.
 *
 * Use this macro directly if you want 'name' and 'key' to be different. For example, this might be
 * the case if 'name' is not a valid name for a C++ symbol (e.g. foo-bar). Otherwise, use
 * REGISTER_GRANULARITY_ROUNDER instead.
 *
 * As an example, if you are adding a rounding granularity called foo-bar you would add this line:
 * REGISTER_GRANULARITY_ROUNDER_GENERAL("foo-bar", foo_bar, GranularityRounderFooBar::create);
 */
#define REGISTER_GRANULARITY_ROUNDER_GENERAL(name, key, rounder)               \
    MONGO_INITIALIZER(addToGranularityRounderMap_##key)(InitializerContext*) { \
        GranularityRounder::registerGranularityRounder(name, rounder);         \
        return Status::OK();                                                   \
    }

/**
 * Registers a GranularityRounder to have the name 'key'. When a granularity specification with name
 * 'key' is found, 'rounder' will be called to construct a GranularityRounder for that
 * granularity.
 *
 * As an example, if you are adding a rounding granularity called foo you would add this line:
 * REGISTER_GRANULARITY_ROUNDER(foo, GranularityRounderFoo::create);
 */
#define REGISTER_GRANULARITY_ROUNDER(key, rounder) \
    REGISTER_GRANULARITY_ROUNDER_GENERAL(#key, key, rounder)

/**
 * This class provides a way to round a value up or down according to a specific rounding
 * granularity.
 */
class GranularityRounder : public RefCountable {
public:
    using Rounder = stdx::function<boost::intrusive_ptr<GranularityRounder>(
        const boost::intrusive_ptr<ExpressionContext>&)>;

    /**
     * Registers a GranularityRounder with a parsing function so that when a granularity
     * with the given name is encountered, it will call 'rounder' to construct the appropriate
     * GranularityRounder.
     *
     * DO NOT call this method directly. Instead, use the REGISTER_GRANULARITY_ROUNDER or
     * REGISTER_GRANULARITY_ROUNDER_GENERAL macro defined in this file.
     */
    static void registerGranularityRounder(StringData granularity, Rounder rounder);

    /**
     * Retrieves the GranularityRounder for the granularity given by 'granularity', and raises an
     * error if there is no such granularity registered.
     */
    static boost::intrusive_ptr<GranularityRounder> getGranularityRounder(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, StringData granularity);

    /**
     * Rounds up 'value' to the first value greater than 'value' in the granularity series. If
     * 'value' is already in the granularity series, then it will be rounded up to the next value in
     * the series.
     */
    virtual Value roundUp(Value value) = 0;

    /**
     * Rounds down 'value' to the first value less than 'value' in the granularity series. If
     * 'value' is already in the granularity series, then it will be rounded down to the previous
     * value in the series.
     */
    virtual Value roundDown(Value value) = 0;

    /**
     * Returns the name of the granularity series that the GranularityRounder is using for rounding.
     */
    virtual std::string getName() = 0;

protected:
    GranularityRounder(const boost::intrusive_ptr<ExpressionContext>& expCtx) : _expCtx(expCtx) {}

    ExpressionContext* getExpCtx() {
        return _expCtx.get();
    }

private:
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

/**
 * This class provides a way to round a value up or down according to a preferred number series.
 *
 * The series that this class aims to support are specified here:
 * https://en.wikipedia.org/wiki/Preferred_number
 */
class GranularityRounderPreferredNumbers final : public GranularityRounder {
public:
    /**
     * Returns a Granularity rounder that rounds according to the series represented by
     * 'baseSeries'. This method requires that baseSeries has at least 2 numbers and is in sorted
     * order.
     */
    static boost::intrusive_ptr<GranularityRounder> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const std::vector<double> baseSeries,
        std::string name);
    Value roundUp(Value value);
    Value roundDown(Value value);

    std::string getName();

    /**
     * Returns a vector that represents the preferred number series that this
     * GranularityRounderPreferredNumbers is using for rounding.
     */
    const std::vector<double> getSeries() const;

private:
    GranularityRounderPreferredNumbers(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       std::vector<double> baseSeries,
                                       std::string name);

    // '_baseSeries' is the preferred number series that is used for rounding. A preferred numbers
    // series is infinite, but we represent it with a finite vector of numbers. When rounding, we
    // scale the numbers in '_baseSeries' by a power of 10 until the number we are rounding falls
    // into the range spanned by the scaled '_baseSeries'.
    const std::vector<double> _baseSeries;
    std::string _name;
};

/**
 * This class provides a way to round a value up or down to a power of two.
 */
class GranularityRounderPowersOfTwo final : public GranularityRounder {
public:
    static boost::intrusive_ptr<GranularityRounder> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx);
    Value roundUp(Value value);
    Value roundDown(Value value);
    std::string getName();

private:
    GranularityRounderPowersOfTwo(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : GranularityRounder(expCtx) {}

    std::string _name = "POWERSOF2";
};
}  //  namespace mongo
