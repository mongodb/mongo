// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/validate/validate_adaptor_factory.h"

#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/validate/validate_adaptor_parallel.h"
#include "mongo/db/validate/validate_adaptor_serial.h"
#include "mongo/db/validate/validate_state.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>

#include <boost/optional/optional.hpp>

namespace mongo::collection_validation {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.factory");

class ValidateAdaptorFactoryTest : public CatalogTestFixture {
protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        ASSERT_OK(storageInterface()->createCollection(operationContext(), kNss, {}));
    }

    std::unique_ptr<ValidateAdaptor> create(boost::optional<int64_t> targetRecordsPerSlice) {
        ValidateState validateState(operationContext(),
                                    kNss,
                                    ValidationOptions(ValidateMode::kForeground,
                                                      RepairMode::kNone,
                                                      /*logDiagnostics=*/false,
                                                      currentValidationVersion,
                                                      /*verifyConfigurationOverride=*/boost::none,
                                                      /*readTimestamp=*/boost::none,
                                                      /*hashPrefixes=*/boost::none,
                                                      /*revealHashedIds=*/boost::none,
                                                      targetRecordsPerSlice));
        return ValidateAdaptorFactory::create(operationContext(), validateState);
    }
};

TEST_F(ValidateAdaptorFactoryTest, CreatesSerialAdaptorWithoutSliceTarget) {
    ASSERT_TRUE(dynamic_cast<ValidateAdaptorSerial*>(create(boost::none).get()));
}

TEST_F(ValidateAdaptorFactoryTest, CreatesSerialAdaptorWhenSliceTargetIsZero) {
    ASSERT_TRUE(dynamic_cast<ValidateAdaptorSerial*>(create(0).get()));
}

TEST_F(ValidateAdaptorFactoryTest, CreatesSerialAdaptorWhenMaxSlicesIsOne) {
    unittest::ServerParameterGuard maxSlicesGuard{"validateParallelMaxRecordStoreSlices", 1};
    ASSERT_TRUE(dynamic_cast<ValidateAdaptorSerial*>(create(10).get()));
}

TEST_F(ValidateAdaptorFactoryTest, CreatesParallelAdaptorWhenSlicingIsEnabled) {
    ASSERT_TRUE(dynamic_cast<ValidateAdaptorParallel*>(create(10).get()));
}

}  // namespace
}  // namespace mongo::collection_validation
