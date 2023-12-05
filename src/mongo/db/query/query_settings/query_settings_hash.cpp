/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/query_settings/query_settings_hash.h"

#include "mongo/db/query/query_knobs_gen.h"

namespace mongo::query_settings {

size_t hash(const QuerySettings& querySettings) {
    size_t hash = 0;
    if (auto version = querySettings.getQueryFramework()) {
        boost::hash_combine(hash, absl::Hash<QueryFrameworkControlEnum>{}(*version));
    }
    if (auto indexHints = querySettings.getIndexHints()) {
        auto hashVectorOfHints = [&](const std::vector<IndexHint>& hints) {
            for (const auto& hint : hints) {
                boost::hash_combine(hash, hint.hash());
            }
        };
        visit(OverloadedVisitor{
                  [&](const std::vector<IndexHintSpec>& hintSpecs) {
                      for (const auto& hintSpec : hintSpecs) {
                          hashVectorOfHints(hintSpec.getAllowedIndexes());
                      }
                  },
                  [&](const IndexHintSpec& hintSpec) {
                      hashVectorOfHints(hintSpec.getAllowedIndexes());
                  },
              },
              *indexHints);
    }
    return hash;
}
}  // namespace mongo::query_settings
