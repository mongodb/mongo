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

#include "mongo/db/local_catalog/index_catalog_entry_helpers.h"

#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/query/planner_ixselect.h"

namespace mongo::index_catalog_helpers {

void computeUpdateIndexData(const IndexCatalogEntry* entry,
                            const IndexAccessMethod* accessMethod,
                            UpdateIndexData* outData) {
    const IndexDescriptor* descriptor = entry->descriptor();
    if (descriptor->getAccessMethodName() == IndexNames::WILDCARD) {
        // Obtain the projection used by the $** index's key generator.
        const auto* pathProj = static_cast<const IndexPathProjection*>(
            static_cast<const WildcardAccessMethod*>(accessMethod)->getWildcardProjection());
        // If the projection is an exclusion, then we must check the new document's keys on all
        // updates, since we do not exhaustively know the set of paths to be indexed.
        if (pathProj->exec()->getType() ==
            TransformerInterface::TransformerType::kExclusionProjection) {
            outData->setAllPathsIndexed();
        } else {
            // If a subtree was specified in the keyPattern, or if an inclusion projection is
            // present, then we need only index the path(s) preserved by the projection.
            const auto& exhaustivePaths = pathProj->exhaustivePaths();
            invariant(exhaustivePaths);
            for (const auto& path : *exhaustivePaths) {
                outData->addPath(path);
            }

            // Handle regular index fields of Compound Wildcard Index.
            BSONObj key = descriptor->keyPattern();
            BSONObjIterator j(key);
            while (j.more()) {
                StringData fieldName(j.next().fieldName());
                if (!fieldName.ends_with("$**"_sd)) {
                    outData->addPath(FieldRef{fieldName});
                }
            }
        }
    } else if (descriptor->getAccessMethodName() == IndexNames::TEXT) {
        fts::FTSSpec ftsSpec(descriptor->infoObj());

        if (ftsSpec.wildcard()) {
            outData->setAllPathsIndexed();
        } else {
            for (size_t i = 0; i < ftsSpec.numExtraBefore(); ++i) {
                outData->addPath(FieldRef(ftsSpec.extraBefore(i)));
            }
            for (fts::Weights::const_iterator it = ftsSpec.weights().begin();
                 it != ftsSpec.weights().end();
                 ++it) {
                outData->addPath(FieldRef(it->first));
            }
            for (size_t i = 0; i < ftsSpec.numExtraAfter(); ++i) {
                outData->addPath(FieldRef(ftsSpec.extraAfter(i)));
            }
            // Any update to a path containing "language" as a component could change the
            // language of a subdocument.  Add the override field as a path component.
            outData->addPathComponent(ftsSpec.languageOverrideField());
        }
    } else {
        BSONObj key = descriptor->keyPattern();
        BSONObjIterator j(key);
        while (j.more()) {
            BSONElement e = j.next();
            outData->addPath(FieldRef(e.fieldName()));
        }
    }

    // handle partial indexes
    const MatchExpression* filter = entry->getFilterExpression();
    if (filter) {
        RelevantFieldIndexMap paths;
        QueryPlannerIXSelect::getFields(filter, &paths);
        for (auto it = paths.begin(); it != paths.end(); ++it) {
            outData->addPath(FieldRef(it->first));
        }
    }
}

}  // namespace mongo::index_catalog_helpers
