/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/extension/host_connector/handle/host_operation_metrics_handle.h"
#include "mongo/db/extension/sdk/extension_operation_metrics_handle.h"
#include "mongo/db/extension/sdk/operation_metrics_adapter.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo::extension {
namespace {

class TestExtensionMetrics : public sdk::OperationMetricsBase {
public:
    static constexpr StringData kStageName = "$testExtensionMetrics";
    BSONObj serialize() const override {
        return BSON(kStageName << _counter);
    }

    void update(MongoExtensionByteView arguments) override {
        int val = *reinterpret_cast<const int*>(arguments.data);
        _counter += val;
    }

private:
    int _counter = 0;
};

TEST(OperationMetricsHandle, canUpdateAndSerialize) {
    // Create metrics.
    auto adapter = new sdk::OperationMetricsAdapter(std::make_unique<TestExtensionMetrics>());

    // Create handles.
    host_connector::HostOperationMetricsHandle hostHandle(adapter);
    sdk::ExtensionOperationMetricsHandle extHandle(adapter);

    // Update the metrics (mimics getNext() call).
    int incr = 2;
    auto byteView = MongoExtensionByteView{reinterpret_cast<const uint8_t*>(&incr), sizeof(int*)};
    extHandle.update(byteView);

    // Check serialize calls.
    auto hostSerializedObj = hostHandle.serialize();
    ASSERT_BSONOBJ_EQ(extHandle.serialize(), hostSerializedObj);
    ASSERT_BSONOBJ_EQ(BSON(TestExtensionMetrics::kStageName << 2), hostSerializedObj);

    // Update the metrics again.
    incr = 3;
    // No need to update the byte view here because it's a pointer to the same int.
    extHandle.update(byteView);

    // Check serialize calls.
    hostSerializedObj = hostHandle.serialize();
    ASSERT_BSONOBJ_EQ(extHandle.serialize(), hostSerializedObj);
    ASSERT_BSONOBJ_EQ(BSON(TestExtensionMetrics::kStageName << 5), hostSerializedObj);
}

}  // namespace
}  // namespace mongo::extension
