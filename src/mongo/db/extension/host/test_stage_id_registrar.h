/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/extension/host/document_source_extension.h"

namespace mongo::extension {

/**
 * RAII helper used in tests to temporarily allocate Ids for stages in unit tests. This removes the
 * need for unit tests to have to fully register stages when they only need to construct
 * DocumentSourceExtensions instead of fully parsing from BSON.
 */
class TestStageIdRegistrar {
public:
    explicit TestStageIdRegistrar(std::initializer_list<StringData> stageNames) {
        for (auto name : stageNames) {
            const std::string key = std::string(name);
            if (host::DocumentSourceExtension::stageToIdMap.find(key) ==
                host::DocumentSourceExtension::stageToIdMap.end()) {
                _inserted.emplace_back(key);
                host::DocumentSourceExtension::stageToIdMap[key] = DocumentSource::allocateId(key);
            }
        }
    }
    ~TestStageIdRegistrar() {
        for (auto& key : _inserted) {
            host::DocumentSourceExtension::stageToIdMap.erase(key);
        }
    }

private:
    std::vector<std::string> _inserted;
};

}  // namespace mongo::extension
