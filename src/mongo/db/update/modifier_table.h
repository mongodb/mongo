// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/update/update_leaf_node.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

namespace mongo {
namespace modifiertable {

// NOTE: Please update jstests/verify_update_mods.js or include a jstest for any new mods
enum ModifierType {
    MOD_ADD_TO_SET,
    MOD_BIT,
    MOD_CURRENTDATE,
    MOD_INC,
    MOD_MAX,
    MOD_MIN,
    MOD_MUL,
    MOD_POP,
    MOD_PULL,
    MOD_PULL_ALL,
    MOD_PUSH,
    MOD_SET,
    MOD_SET_ON_INSERT,
    MOD_RENAME,
    MOD_UNSET,
    MOD_CONFLICT_PLACEHOLDER,
    MOD_UNKNOWN
};

/**
 * Returns the modifier type for 'typeStr', if it was recognized as an existing update
 * mod, or MOD_UNKNOWN otherwise.
 */
ModifierType getType(std::string_view typeStr);

/**
 * Instantiate an UpdateLeafNode that corresponds to 'modType' or nullptr if 'modType' is not valid.
 */
std::unique_ptr<UpdateLeafNode> makeUpdateLeafNode(ModifierType modType);

}  // namespace modifiertable
}  // namespace mongo
