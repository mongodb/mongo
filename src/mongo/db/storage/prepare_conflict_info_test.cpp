// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/prepare_conflict_info.h"

#include "mongo/base/status.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstdint>
#include <limits>
#include <string>

namespace mongo {
namespace {

TEST(PrepareConflictInfoTest, GenerationSurvivesStatusAndSerialization) {
    for (auto generation :
         {uint64_t{0}, uint64_t{42}, static_cast<uint64_t>(std::numeric_limits<int64_t>::max())}) {
        SCOPED_TRACE("Generation: " + std::to_string(generation));

        Status status = Status::OK();
        try {
            uasserted(PrepareConflictInfo(generation), "Injected prepare conflict");
        } catch (const ExceptionFor<ErrorCodes::PrepareConflict>& ex) {
            status = ex.toStatus();
        }
        ASSERT_EQ(status.code(), ErrorCodes::PrepareConflict);
        ASSERT_EQ(status.extraInfo<PrepareConflictInfo>()->generation(), generation);

        BSONObjBuilder builder;
        status.extraInfo()->serialize(&builder);
        auto serialized = builder.obj();
        EXPECT_EQ(serialized["generation"].type(), BSONType::numberLong);
        EXPECT_EQ(serialized["generation"].Long(), static_cast<int64_t>(generation));

        Status parsed{ErrorCodes::PrepareConflict, status.reason(), serialized};
        ASSERT_EQ(parsed.code(), ErrorCodes::PrepareConflict);
        try {
            uassertStatusOK(parsed);
            FAIL("Expected a prepare conflict");
        } catch (const ExceptionFor<ErrorCodes::PrepareConflict>& ex) {
            EXPECT_EQ(ex->generation(), generation);
        }
    }
}

TEST(PrepareConflictInfoTest, RejectMalformedGeneration) {
    for (const auto& obj : {BSONObj{},
                            BSON("generation" << -1LL),
                            BSON("generation" << 42),
                            BSON("generation" << 42.0),
                            BSON("generation" << "42")}) {
        EXPECT_THAT([&] { PrepareConflictInfo::parse(obj); }, testing::Throws<DBException>())
            << "Input: " << obj;
    }
}

TEST(PrepareConflictInfoTest, RejectGenerationOutsideSignedRange) {
    auto firstOutOfRange = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1;
    for (auto generation : {firstOutOfRange, std::numeric_limits<uint64_t>::max()}) {
        EXPECT_THAT([&] { PrepareConflictInfo{generation}; },
                    testing::Throws<DBException>(unittest::match::CodeIs(ErrorCodes::BadValue)))
            << "Generation: " << generation;
    }
}

}  // namespace
}  // namespace mongo
