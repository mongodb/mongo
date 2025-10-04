/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/update/update_leaf_node.h"

#include <memory>

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
ModifierType getType(StringData typeStr);

/**
 * Instantiate an UpdateLeafNode that corresponds to 'modType' or nullptr if 'modType' is not valid.
 */
std::unique_ptr<UpdateLeafNode> makeUpdateLeafNode(ModifierType modType);

}  // namespace modifiertable
}  // namespace mongo
