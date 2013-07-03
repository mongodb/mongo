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

#include "mongo/db/ops/modifier_table.h"

#include <string>
#include <utility>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/ops/modifier_add_to_set.h"
#include "mongo/db/ops/modifier_bit.h"
#include "mongo/db/ops/modifier_inc.h"
#include "mongo/db/ops/modifier_pop.h"
#include "mongo/db/ops/modifier_pull.h"
#include "mongo/db/ops/modifier_pull_all.h"
#include "mongo/db/ops/modifier_push.h"
#include "mongo/db/ops/modifier_rename.h"
#include "mongo/db/ops/modifier_set.h"
#include "mongo/db/ops/modifier_unset.h"
#include "mongo/platform/unordered_map.h"

namespace mongo {
namespace modifiertable {

    namespace {

        struct ModifierEntry {
            string name;
            ModifierType type;

            ModifierEntry(const StringData& name, ModifierType type)
                : name(name.toString())
                , type(type) {
            }
        };

        typedef unordered_map<StringData, ModifierEntry*, StringData::Hasher> NameMap;

        NameMap* MODIFIER_NAME_MAP;

        void init(NameMap* nameMap) {
            ModifierEntry* entryAddToSet = new ModifierEntry("$addToSet", MOD_ADD_TO_SET);
            nameMap->insert(make_pair(StringData(entryAddToSet->name), entryAddToSet));

            ModifierEntry* entryBit = new ModifierEntry("$bit", MOD_BIT);
            nameMap->insert(make_pair(StringData(entryBit->name), entryBit));

            ModifierEntry* entryPop = new ModifierEntry("$pop", MOD_POP);
            nameMap->insert(make_pair(StringData(entryPop->name), entryPop));

            ModifierEntry* entryPull = new ModifierEntry("$pull", MOD_PULL);
            nameMap->insert(make_pair(StringData(entryPull->name), entryPull));

            ModifierEntry* entryPullAll = new ModifierEntry("$pullAll", MOD_PULL_ALL);
            nameMap->insert(make_pair(StringData(entryPullAll->name), entryPullAll));

            ModifierEntry* entryPush = new ModifierEntry("$push", MOD_PUSH);
            nameMap->insert(make_pair(StringData(entryPush->name), entryPush));

            ModifierEntry* entryPushAll = new ModifierEntry("$pushAll", MOD_PUSH_ALL);
            nameMap->insert(make_pair(StringData(entryPushAll->name), entryPushAll));

            ModifierEntry* entryInc = new ModifierEntry("$inc", MOD_INC);
            nameMap->insert(make_pair(StringData(entryInc->name), entryInc));

            ModifierEntry* entrySet = new ModifierEntry("$set", MOD_SET);
            nameMap->insert(make_pair(StringData(entrySet->name), entrySet));

            ModifierEntry* entrySetOnInsert = new ModifierEntry("$setOnInsert", MOD_SET_ON_INSERT);
            nameMap->insert(make_pair(StringData(entrySetOnInsert->name), entrySetOnInsert));

            ModifierEntry* entryRename = new ModifierEntry("$rename", MOD_RENAME);
            nameMap->insert(make_pair(StringData(entryRename->name), entryRename));

            ModifierEntry* entryUnset = new ModifierEntry("$unset", MOD_UNSET);
            nameMap->insert(make_pair(StringData(entryUnset->name), entryUnset));
        }

    } // unnamed namespace

    MONGO_INITIALIZER(ModifierTable)(InitializerContext* context) {
        MODIFIER_NAME_MAP = new NameMap;
        init(MODIFIER_NAME_MAP);

        return Status::OK();
    }

    ModifierType getType(const StringData& typeStr) {
        NameMap::const_iterator it = MODIFIER_NAME_MAP->find(typeStr);
        if (it == MODIFIER_NAME_MAP->end()) {
            return MOD_UNKNOWN;
        }
        return it->second->type;
    }

    ModifierInterface* makeUpdateMod(ModifierType modType) {
        switch (modType) {
        case MOD_ADD_TO_SET:
            return new ModifierAddToSet;
        case MOD_BIT:
            return new ModifierBit;
        case MOD_INC:
            return new ModifierInc;
        case MOD_POP:
            return new ModifierPop;
        case MOD_PULL:
            return new ModifierPull;
        case MOD_PULL_ALL:
            return new ModifierPullAll;
        case MOD_PUSH:
            return new ModifierPush(ModifierPush::PUSH_NORMAL);
        case MOD_PUSH_ALL:
            return new ModifierPush(ModifierPush::PUSH_ALL);
        case MOD_SET:
            return new ModifierSet;
        case MOD_SET_ON_INSERT:
            verify(false);
            return NULL; // TODO (syntactic sugar)
        case MOD_RENAME:
            return new ModifierRename;
        case MOD_UNSET:
            return new ModifierUnset;
        default:
            return NULL;
        }
    }

} // namespace modifiertable
} // namespace mongo
