// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/sdk/operation_metrics_adapter.h"
#include "mongo/db/extension/shared/handle/operation_metrics_handle.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <string_view>

namespace mongo::extension {
namespace {

class TestExtensionMetrics : public sdk::OperationMetricsBase {
public:
    static constexpr std::string_view kStageName = "$testExtensionMetrics";
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

TEST(OwnedOperationMetricsHandle, canUpdateAndSerialize) {
    // Create metrics.
    auto adapter = new sdk::OperationMetricsAdapter(std::make_unique<TestExtensionMetrics>());

    // Create handles.
    OwnedOperationMetricsHandle hostHandle(adapter);
    UnownedOperationMetricsHandle extHandle(adapter);

    // Update the metrics (mimics getNext() call).
    int incr = 2;
    auto byteView = MongoExtensionByteView{reinterpret_cast<const uint8_t*>(&incr), sizeof(int*)};
    extHandle->update(byteView);

    // Check serialize calls.
    auto hostSerializedObj = hostHandle->serialize();
    ASSERT_BSONOBJ_EQ(extHandle->serialize(), hostSerializedObj);
    ASSERT_BSONOBJ_EQ(BSON(TestExtensionMetrics::kStageName << 2), hostSerializedObj);

    // Update the metrics again.
    incr = 3;
    // No need to update the byte view here because it's a pointer to the same int.
    extHandle->update(byteView);

    // Check serialize calls.
    hostSerializedObj = hostHandle->serialize();
    ASSERT_BSONOBJ_EQ(extHandle->serialize(), hostSerializedObj);
    ASSERT_BSONOBJ_EQ(BSON(TestExtensionMetrics::kStageName << 5), hostSerializedObj);
}

}  // namespace
}  // namespace mongo::extension
