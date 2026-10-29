// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/index_catalog_entry_helpers.h"

#include "mongo/db/index/wildcard_access_method.h"
#include "mongo/db/query/planner_ixselect.h"

#include <string_view>

namespace mongo::index_catalog_helpers {
using namespace std::literals::string_view_literals;

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
                std::string_view fieldName(j.next().fieldName());
                if (!fieldName.ends_with("$**"sv)) {
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
