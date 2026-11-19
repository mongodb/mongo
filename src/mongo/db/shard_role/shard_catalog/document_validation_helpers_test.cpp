// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/document_validation_helpers.h"

#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <boost/none.hpp>
#include <boost/optional.hpp>

namespace mongo {
namespace {

// Builds the options of an already-existing collection (uuid set) so the upgrade checks are
// exercised rather than the create-collection path.
CollectionOptions makeExistingCollection(ValidationLevelEnum level, ValidationActionEnum action) {
    CollectionOptions opts;
    opts.uuid = UUID::gen();
    opts.validationLevel = level;
    opts.validationAction = action;
    return opts;
}

TEST(DocumentValidationHelpers, PrepareConstraintRequiresNonWarnAction) {
    auto opts = makeExistingCollection(ValidationLevelEnum::strict, ValidationActionEnum::warn);
    ASSERT_NOT_OK(checkValidationOptionsCanBeUsed(
        opts, boost::none, boost::none, boost::none, /*newPrepareConstraintValidationLevel=*/true));
}

TEST(DocumentValidationHelpers, PrepareConstraintAllowedWithErrorAction) {
    auto opts = makeExistingCollection(ValidationLevelEnum::strict, ValidationActionEnum::error);
    ASSERT_OK(checkValidationOptionsCanBeUsed(
        opts, boost::none, boost::none, boost::none, /*newPrepareConstraintValidationLevel=*/true));
}

TEST(DocumentValidationHelpers, PrepareConstraintAllowedWithErrorAndLogAction) {
    auto opts =
        makeExistingCollection(ValidationLevelEnum::strict, ValidationActionEnum::errorAndLog);
    ASSERT_OK(checkValidationOptionsCanBeUsed(
        opts, boost::none, boost::none, boost::none, /*newPrepareConstraintValidationLevel=*/true));
}

TEST(DocumentValidationHelpers, PrepareConstraintAllowedWhenWarnIsChangedToErrorInSameCommand) {
    auto opts = makeExistingCollection(ValidationLevelEnum::strict, ValidationActionEnum::warn);
    ASSERT_OK(checkValidationOptionsCanBeUsed(opts,
                                              boost::none,
                                              ValidationActionEnum::error,
                                              boost::none,
                                              /*newPrepareConstraintValidationLevel=*/true));
}

TEST(DocumentValidationHelpers, PrepareConstraintFalseDoesNotRequireActionCheck) {
    auto opts = makeExistingCollection(ValidationLevelEnum::strict, ValidationActionEnum::warn);
    ASSERT_OK(checkValidationOptionsCanBeUsed(opts,
                                              boost::none,
                                              boost::none,
                                              boost::none,
                                              /*newPrepareConstraintValidationLevel=*/false));
}

TEST(DocumentValidationHelpers, UpgradeToConstraintBlockedWhenPreviousActionWasWarn) {
    auto opts = makeExistingCollection(ValidationLevelEnum::strict, ValidationActionEnum::warn);
    opts.prepareConstraintValidationLevel = true;
    ASSERT_NOT_OK(checkValidationOptionsCanBeUsed(opts,
                                                  ValidationLevelEnum::constraint,
                                                  ValidationActionEnum::error,
                                                  boost::none,
                                                  boost::none));
}

TEST(DocumentValidationHelpers, UpgradeToConstraintAllowedWhenPreviousActionWasError) {
    auto opts = makeExistingCollection(ValidationLevelEnum::strict, ValidationActionEnum::error);
    opts.prepareConstraintValidationLevel = true;
    ASSERT_OK(checkValidationOptionsCanBeUsed(
        opts, ValidationLevelEnum::constraint, boost::none, boost::none, boost::none));
}

TEST(DocumentValidationHelpers, UpgradeToConstraintBlockedWhenNewActionIsWarn) {
    auto opts = makeExistingCollection(ValidationLevelEnum::strict, ValidationActionEnum::error);
    opts.prepareConstraintValidationLevel = true;
    ASSERT_NOT_OK(checkValidationOptionsCanBeUsed(opts,
                                                  ValidationLevelEnum::constraint,
                                                  ValidationActionEnum::warn,
                                                  boost::none,
                                                  boost::none));
}

TEST(DocumentValidationHelpers, UpgradeToConstraintAllowedWithErrorAndLogAction) {
    auto opts = makeExistingCollection(ValidationLevelEnum::strict, ValidationActionEnum::error);
    opts.prepareConstraintValidationLevel = true;
    ASSERT_OK(checkValidationOptionsCanBeUsed(opts,
                                              ValidationLevelEnum::constraint,
                                              ValidationActionEnum::errorAndLog,
                                              boost::none,
                                              boost::none));
}

TEST(DocumentValidationHelpers, UpgradeToConstraintBlockedWithoutPrepare) {
    auto opts = makeExistingCollection(ValidationLevelEnum::strict, ValidationActionEnum::error);
    ASSERT_NOT_OK(checkValidationOptionsCanBeUsed(
        opts, ValidationLevelEnum::constraint, boost::none, boost::none, boost::none));
}

}  // namespace
}  // namespace mongo
