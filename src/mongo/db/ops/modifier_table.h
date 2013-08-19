/**
 *    Copyright (C) 2013 10gen Inc.
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
 */

#pragma once

#include "mongo/db/ops/modifier_interface.h"

namespace mongo {
namespace modifiertable {

    enum ModifierType {
        MOD_ADD_TO_SET,
        MOD_BIT,
        MOD_INC,
        MOD_MUL,
        MOD_POP,
        MOD_PULL,
        MOD_PULL_ALL,
        MOD_PUSH,
        MOD_PUSH_ALL,
        MOD_SET,
        MOD_SET_ON_INSERT,
        MOD_RENAME,
        MOD_UNSET,
        MOD_UNKNOWN
    };

    /**
     * Returns the modifier type for 'typeStr', if it was recognized as an existing update
     * mod, or MOD_UNKNOWN otherwise.
     */
    ModifierType getType(const StringData& typeStr);

    /**
     * Instantiate an update mod that corresponds to 'modType' or NULL if 'modType' is not
     * valid. The ownership of the new object is the caller's.
     */
    ModifierInterface* makeUpdateMod(ModifierType modType);

} // namespace modifiertable
} // namespace mongo
