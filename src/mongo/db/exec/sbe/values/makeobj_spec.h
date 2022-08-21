/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/makeobj_enums.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/value.h"

namespace mongo::sbe::value {
/**
 * MakeObjSpec is a wrapper around a FieldBehavior value, a list of fields, a list of projected
 * fields, and a StringMap object.
 */
class MakeObjSpec {
public:
    using FieldBehavior = MakeObjFieldBehavior;

    // GetArgFn is a function pointer type used by produceBsonObject().
    using GetArgFn = std::function<std::pair<value::TypeTags, value::Value>(size_t idx)>;

    MakeObjSpec(boost::optional<FieldBehavior> fieldBehavior,
                std::vector<std::string> fields,
                std::vector<std::string> projectFields)
        : _fieldBehavior(fieldBehavior),
          _fields(fields),
          _projectFields(projectFields),
          _allFieldsMap(buildAllFieldsMap()) {}

    size_t getApproximateSize() const;

private:
    StringMap<size_t> buildAllFieldsMap() const;

    void keepOrDropFields(value::TypeTags rootTag,
                          value::Value rootVal,
                          UniqueBSONObjBuilder& bob) const;

public:
    static StringMap<size_t> buildAllFieldsMap(const std::vector<std::string>& fields,
                                               const std::vector<std::string>& projectFields) {
        StringMap<size_t> m;

        for (auto& p : fields) {
            // Mark the values from fields with 'std::numeric_limits<size_t>::max()'.
            auto [it, inserted] = m.emplace(p, std::numeric_limits<size_t>::max());
            uassert(6897000, str::stream() << "duplicate field: " << p, inserted);
        }

        for (size_t idx = 0; idx < projectFields.size(); ++idx) {
            auto& p = projectFields[idx];
            // Mark the values from projectFields with their corresponding index.
            auto [it, inserted] = m.emplace(p, idx);
            uassert(6897001, str::stream() << "duplicate field: " << p, inserted);
        }

        return m;
    }

    static void keepOrDropFields(value::TypeTags rootTag,
                                 value::Value rootVal,
                                 const boost::optional<FieldBehavior>& fieldBehavior,
                                 size_t nFieldsNeededIfInclusion,
                                 const StringMap<size_t>& allFieldsMap,
                                 UniqueBSONObjBuilder& bob) {
        auto isFieldProjectedOrRestricted = [&](const StringMapHashedKey& key) -> bool {
            bool foundKey = false;
            bool projected = false;
            bool restricted = false;

            if (!allFieldsMap.empty()) {
                if (auto it = allFieldsMap.find(key); it != allFieldsMap.end()) {
                    foundKey = true;
                    projected = it->second != std::numeric_limits<size_t>::max();
                    restricted = *fieldBehavior != FieldBehavior::keep;
                }
            }
            if (!foundKey) {
                restricted = *fieldBehavior == FieldBehavior::keep;
            }

            return projected || restricted;
        };

        if (rootTag == value::TypeTags::bsonObject) {
            if (!(nFieldsNeededIfInclusion == 0 && fieldBehavior == FieldBehavior::keep)) {
                auto be = value::bitcastTo<const char*>(rootVal);
                // Skip document length.
                be += 4;
                while (*be != 0) {
                    auto sv = bson::fieldNameView(be);
                    auto key = StringMapHasher{}.hashed_key(StringData(sv));
                    auto nextBe = bson::advance(be, sv.size());

                    if (!isFieldProjectedOrRestricted(key)) {
                        bob.append(BSONElement(be, sv.size() + 1, nextBe - be));
                        --nFieldsNeededIfInclusion;
                    }

                    if (nFieldsNeededIfInclusion == 0 && fieldBehavior == FieldBehavior::keep) {
                        break;
                    }

                    be = nextBe;
                }
            }
        } else if (rootTag == value::TypeTags::Object) {
            if (!(nFieldsNeededIfInclusion == 0 && fieldBehavior == FieldBehavior::keep)) {
                auto objRoot = value::getObjectView(rootVal);
                for (size_t idx = 0; idx < objRoot->size(); ++idx) {
                    auto key = StringMapHasher{}.hashed_key(StringData(objRoot->field(idx)));

                    if (!isFieldProjectedOrRestricted(key)) {
                        auto [tag, val] = objRoot->getAt(idx);
                        bson::appendValueToBsonObj(bob, objRoot->field(idx), tag, val);
                        --nFieldsNeededIfInclusion;
                    }

                    if (nFieldsNeededIfInclusion == 0 && fieldBehavior == FieldBehavior::keep) {
                        break;
                    }
                }
            }
        }
    }

    // This method will invoke the 'getArg' function pointer to retrieve the value for each
    // projected field (if any).
    std::pair<value::TypeTags, value::Value> produceBsonObject(value::TypeTags rootTag,
                                                               value::Value rootVal,
                                                               const GetArgFn& getArg) const {
        UniqueBSONObjBuilder bob;
        if (value::isObject(rootTag)) {
            keepOrDropFields(rootTag, rootVal, bob);
        }

        for (size_t idx = 0; idx < _projectFields.size(); ++idx) {
            auto [fieldTag, fieldVal] = getArg(idx);
            bson::appendValueToBsonObj(bob, _projectFields[idx], fieldTag, fieldVal);
        }

        bob.doneFast();
        char* data = bob.bb().release().release();
        return {value::TypeTags::bsonObject, value::bitcastFrom<char*>(data)};
    }

    std::string toString() const;

private:
    const boost::optional<FieldBehavior> _fieldBehavior;
    const std::vector<std::string> _fields;
    const std::vector<std::string> _projectFields;
    const StringMap<size_t> _allFieldsMap;
};
}  // namespace mongo::sbe::value
