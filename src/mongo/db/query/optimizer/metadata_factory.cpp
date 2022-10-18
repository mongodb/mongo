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

#include "mongo/db/query/optimizer/metadata_factory.h"

#include "mongo/db/query/optimizer/rewrites/const_eval.h"
#include "mongo/db/query/optimizer/utils/utils.h"


namespace mongo::optimizer {

ScanDefinition createScanDef(ScanDefOptions options,
                             opt::unordered_map<std::string, IndexDefinition> indexDefs) {
    return createScanDef(std::move(options),
                         std::move(indexDefs),
                         ConstEval::constFold,
                         {DistributionType::Centralized},
                         true /*exists*/);
}

ScanDefinition createScanDef(ScanDefOptions options,
                             opt::unordered_map<std::string, IndexDefinition> indexDefs,
                             const ConstFoldFn& constFold,
                             DistributionAndPaths distributionAndPaths,
                             const bool exists,
                             const CEType ce) {
    IndexPathSet nonMultiKeyPathSet;

    // Collect non-multiKeyPaths from each index.
    for (const auto& [indexDefName, indexDef] : indexDefs) {
        for (const auto& collation : indexDef.getCollationSpec()) {
            if (!checkPathContainsTraverse(collation._path)) {
                nonMultiKeyPathSet.insert(collation._path);
            }
        }
    }

    // Simplify partial filter requirements using the non-multikey paths.
    for (auto& [indexDefName, indexDef] : indexDefs) {
        [[maybe_unused]] const bool hasEmptyInterval = simplifyPartialSchemaReqPaths(
            "" /*scanProjName*/, nonMultiKeyPathSet, indexDef.getPartialReqMap(), constFold);
        // If "hasEmptyInterval" is set, we have a partial filter index with an unsatisfiable
        // condition, which is thus guaranteed to never contain any documents.
    }

    return {std::move(options),
            std::move(indexDefs),
            std::move(nonMultiKeyPathSet),
            std::move(distributionAndPaths),
            exists,
            ce};
}

}  // namespace mongo::optimizer
