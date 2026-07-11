// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/modules.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

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
    using Rounder = std::function<boost::intrusive_ptr<GranularityRounder>(
        const boost::intrusive_ptr<ExpressionContext>&)>;

    /**
     * Registers a GranularityRounder with a parsing function so that when a granularity
     * with the given name is encountered, it will call 'rounder' to construct the appropriate
     * GranularityRounder.
     *
     * DO NOT call this method directly. Instead, use the REGISTER_GRANULARITY_ROUNDER or
     * REGISTER_GRANULARITY_ROUNDER_GENERAL macro defined in this file.
     */
    static void registerGranularityRounder(std::string_view granularity, Rounder rounder);

    /**
     * Retrieves the GranularityRounder for the granularity given by 'granularity', and raises an
     * error if there is no such granularity registered.
     */
    static boost::intrusive_ptr<GranularityRounder> getGranularityRounder(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, std::string_view granularity);

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
        std::vector<double> baseSeries,
        std::string name);
    Value roundUp(Value value) override;
    Value roundDown(Value value) override;

    std::string getName() override;

    /**
     * Returns a vector that represents the preferred number series that this
     * GranularityRounderPreferredNumbers is using for rounding.
     */
    std::vector<double> getSeries() const;

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
    Value roundUp(Value value) override;
    Value roundDown(Value value) override;
    std::string getName() override;

private:
    GranularityRounderPowersOfTwo(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : GranularityRounder(expCtx) {}

    std::string _name = "POWERSOF2";
};
}  //  namespace mongo
