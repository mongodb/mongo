// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/extension/shared/extension_agg_stage_static_properties_validator.h"

#include "mongo/db/extension/public/extension_agg_stage_static_properties_gen.h"
#include "mongo/util/assert_util.h"

namespace mongo::extension {

void validateDocsNeededBoundsInfo(const MongoExtensionDocsNeededBoundsInfo* info) {
    const bool requiresValue =
        info->getEffect() == MongoExtensionDocsNeededBoundsEffectEnum::kLimit ||
        info->getEffect() == MongoExtensionDocsNeededBoundsEffectEnum::kSkip;
    tassert(ErrorCodes::ExtensionError,
            "'value' must be specified when 'effect' is 'limit' or 'skip'",
            !requiresValue || info->getValue().has_value());
    tassert(ErrorCodes::ExtensionError,
            "'value' must only be specified when 'effect' is 'limit' or 'skip'",
            requiresValue || !info->getValue().has_value());
}

}  // namespace mongo::extension
