/**
 *    Copyright (C) 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

/**
 * Tests for mongo/util/descriptive_stats.h
 */

#include <cstdlib>
#include <cmath>
#include <limits>
#include <string>

#include "mongo/unittest/unittest.h"
#include "mongo/util/descriptive_stats.h"

using namespace std;

namespace {

    const size_t NumQuantiles = 99;

    TEST(DistributionEstimators, TestNominalResults) {
        mongo::DistributionEstimators<NumQuantiles> d;

        for (int i = 0; i < 100000; i++) {
            d << double(i) / 100000;
        }
        ASSERT_TRUE(d.quantilesReady());
        for (size_t quant = 1; quant <= NumQuantiles; ++quant) {
            ASSERT_EQUALS(d.probability(quant), double(quant) / 100);
            ASSERT_APPROX_EQUAL(d.quantile(quant), double(quant) / 100, 0.05);
            double prob = double(quant) / 100;
            ASSERT_APPROX_EQUAL(d.icdf(prob), prob, 0.05);
        }
        ASSERT_APPROX_EQUAL(d.min(), 0.0, 0.05);
        ASSERT_APPROX_EQUAL(d.max(), 1.0, 0.05);
        ASSERT_APPROX_EQUAL(d.median(), 0.5, 0.05);
    }

    TEST(DistributionEstimators, TestAppendQuantilesToBSONArrayBuilder) {
        mongo::DistributionEstimators<NumQuantiles> d;

        for (int i = 0; i < 10000; i++) {
            d << static_cast<double>(i) / 10000;
        }

        mongo::BSONArrayBuilder arrayBuilder;
        d.appendQuantilesToBSONArrayBuilder(arrayBuilder);
        mongo::BSONArray arr = arrayBuilder.arr();

        for (size_t i = 0; i <= NumQuantiles + 1; i++) {
            ASSERT_EQUALS(arr[i].Number(), d.quantile(i));
        }
    }

    TEST(BasicEstimators, TestNominalResults) {
        mongo::BasicEstimators<unsigned int> d;

        unsigned int count = 0;
        // [50, 51, 52, ..., 99949, 99950]
        for (int i = 50; i <= 100000 - 50; i++) {
            d << unsigned(i);
            count++;
        }
        ASSERT_EQUALS(d.min(), 50u);
        ASSERT_EQUALS(d.max(), 100000u - 50u);
        ASSERT_APPROX_EQUAL(d.mean(), 100000 / 2, 1e-15);
        ASSERT_APPROX_EQUAL(d.stddev(), sqrt((static_cast<double>(count) * count - 1) / 12), 1e-15);
    }

    TEST(BasicEstimators, TestAppendBasicToBSONObjBuilder) {
        mongo::BasicEstimators<unsigned int> b;

        for (int i = 0; i < 10000; i++) {
            b << i;
        }

        mongo::BSONObjBuilder builder;
        b.appendBasicToBSONObjBuilder(builder);
        mongo::BSONObj obj = builder.obj();

        ASSERT_EQUALS(obj["count"].Number(), b.count());
        ASSERT_EQUALS(obj["mean"].Number(), b.mean());
        ASSERT_EQUALS(obj["stddev"].Number(), b.stddev());
        ASSERT_EQUALS(obj["min"].Number(), b.min());
        ASSERT_EQUALS(obj["max"].Number(), b.max());
    }

    TEST(SummaryEstimators, TestNominalResults) {
        mongo::SummaryEstimators<int, NumQuantiles> d;

        for (int a = -200; a <= 200; a++) {
            d << a;
        }
        ASSERT_TRUE(d.quantilesReady());
        for (size_t i = 0; i < d.numberOfQuantiles; i++) {
            ASSERT_APPROX_EQUAL(d.quantile(i), -200 + static_cast<int>(i) * 4, 1);
        }
        ASSERT_EQUALS(d.min(), -200);
        ASSERT_EQUALS(d.max(), 200);
        ASSERT_APPROX_EQUAL(d.mean(), 0, 1e-15);
        ASSERT_APPROX_EQUAL(d.icdf(.25), -100, 1e-15);
    }

    TEST(SummaryEstimators, TestStatisticSummaryToBSONObj) {
        mongo::SummaryEstimators<double, NumQuantiles> e;

        for (int i = 0; i < 10000; i++) {
            e << static_cast<double>(i) / 100;
        }
        verify(e.quantilesReady());

        mongo::BSONObj obj = e.statisticSummaryToBSONObj();

        ASSERT_EQUALS(obj["count"].Number(), e.count());
        ASSERT_EQUALS(obj["mean"].Number(), e.mean());
        ASSERT_EQUALS(obj["stddev"].Number(), e.stddev());
        ASSERT_EQUALS(obj["min"].Number(), e.min());
        ASSERT_EQUALS(obj["max"].Number(), e.max());

        mongo::BSONObj quantiles = obj["quantiles"].Obj();
        ASSERT_EQUALS(static_cast<size_t>(quantiles.nFields()), NumQuantiles);
        for (mongo::BSONObjIterator it = quantiles.begin(); it.more(); ++it) {
            ASSERT_EQUALS((*it).Number(), e.icdf(atof((*it).fieldName())));
        }
    }

}  // namespace

