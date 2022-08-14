/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/commands/server_status_metric.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class MetricTreeTest : public unittest::Test {
protected:
    MetricTree* tree() const {
        return _tree.get();
    }

    BSONObj metrics() {
        BSONObjBuilder builder;
        tree()->appendTo(builder);
        return builder.obj();
    }

    std::unique_ptr<MetricTree> _tree = std::make_unique<MetricTree>();
};

TEST_F(MetricTreeTest, ValidateCounterMetric) {
    auto& counter =
        addMetricToTree(std::make_unique<ServerStatusMetricField<Counter64>>("tree.counter"),
                        tree())
            .value();

    counter.increment();
    const auto firstMetrics = metrics();
    BSONElement counterElement = firstMetrics["metrics"]["tree"]["counter"];
    ASSERT(counterElement.isNumber());
    ASSERT_EQ(counterElement.numberInt(), counter.get());

    counter.increment(2);

    const auto secondMetrics = metrics();
    BSONElement anotherCounterElement = secondMetrics["metrics"]["tree"]["counter"];
    ASSERT(anotherCounterElement.isNumber());
    ASSERT_EQ(anotherCounterElement.numberInt(), counter.get());
}

TEST_F(MetricTreeTest, ValidateTextMetric) {
    auto& text =
        addMetricToTree(std::make_unique<ServerStatusMetricField<std::string>>("tree.text"), tree())
            .value();

    text = "hello";

    auto firstMetrics = metrics();
    BSONElement textElement = firstMetrics["metrics"]["tree"]["text"];
    ASSERT_EQ(textElement.type(), BSONType::String);
    ASSERT_EQ(textElement.String(), text);

    text = "bye";

    auto secondMetrics = metrics();
    BSONElement anotherTextElement = secondMetrics["metrics"]["tree"]["text"];
    ASSERT_EQ(anotherTextElement.type(), BSONType::String);
    ASSERT_EQ(anotherTextElement.String(), text);
}
}  // namespace
}  // namespace mongo
