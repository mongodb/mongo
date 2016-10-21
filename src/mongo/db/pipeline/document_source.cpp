/**
*    Copyright (C) 2011 10gen Inc.
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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source.h"

#include "mongo/db/matcher/expression_algo.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/util/string_map.h"

namespace mongo {

using Parser = DocumentSource::Parser;
using boost::intrusive_ptr;
using std::string;
using std::vector;

DocumentSource::DocumentSource(const intrusive_ptr<ExpressionContext>& pCtx)
    : pSource(NULL), pExpCtx(pCtx) {}

namespace {
// Used to keep track of which DocumentSources are registered under which name.
static StringMap<Parser> parserMap;
}  // namespace

void DocumentSource::registerParser(string name, Parser parser) {
    auto it = parserMap.find(name);
    massert(28707,
            str::stream() << "Duplicate document source (" << name << ") registered.",
            it == parserMap.end());
    parserMap[name] = parser;
}

vector<intrusive_ptr<DocumentSource>> DocumentSource::parse(
    const intrusive_ptr<ExpressionContext> expCtx, BSONObj stageObj) {
    uassert(16435,
            "A pipeline stage specification object must contain exactly one field.",
            stageObj.nFields() == 1);
    BSONElement stageSpec = stageObj.firstElement();
    auto stageName = stageSpec.fieldNameStringData();

    // Get the registered parser and call that.
    auto it = parserMap.find(stageName);

    uassert(16436,
            str::stream() << "Unrecognized pipeline stage name: '" << stageName << "'",
            it != parserMap.end());

    return it->second(stageSpec, expCtx);
}

const char* DocumentSource::getSourceName() const {
    static const char unknown[] = "[UNKNOWN]";
    return unknown;
}

void DocumentSource::setSource(DocumentSource* pTheSource) {
    verify(!isValidInitialSource());
    pSource = pTheSource;
}

intrusive_ptr<DocumentSource> DocumentSource::optimize() {
    return this;
}

namespace {

/**
 * Given a set of paths 'dependencies', determines which of those paths will be modified if all
 * paths except those in 'preservedPaths' are modified.
 *
 * For example, extractModifiedDependencies({'a', 'b', 'c.d', 'e'}, {'a', 'b.c', c'}) returns
 * {'b', 'e'}, since 'b' and 'e' are not preserved (only 'b.c' is preserved).
 */
std::set<std::string> extractModifiedDependencies(const std::set<std::string>& dependencies,
                                                  const std::set<std::string>& preservedPaths) {
    std::set<std::string> modifiedDependencies;

    // The modified dependencies is *almost* the set difference 'dependencies' - 'preservedPaths',
    // except that if p in 'preservedPaths' is a "path prefix" of d in 'dependencies', then 'd'
    // should not be included in the modified dependencies.
    for (auto&& dependency : dependencies) {
        bool preserved = false;
        auto depAsPath = FieldPath(dependency);
        auto firstField = depAsPath.getFieldName(0);
        // If even a prefix is preserved, the path is preserved, so search for any prefixes of
        // 'dependency' as well. 'preservedPaths' is an *ordered* set, so we only have to search the
        // range ['firstField', 'dependency'] to find any prefixes of 'dependency'.
        for (auto it = preservedPaths.lower_bound(firstField);
             it != preservedPaths.upper_bound(dependency);
             ++it) {
            if (*it == dependency || expression::isPathPrefixOf(*it, dependency)) {
                preserved = true;
                break;
            }
        }
        if (!preserved) {
            modifiedDependencies.insert(dependency);
        }
    }
    return modifiedDependencies;
}

/**
 * Returns a pair of pointers to $match stages, either of which can be null. The first entry in the
 * pair is a $match stage that can be moved before this stage, the second is a $match stage that
 * must remain after this stage.
 */
std::pair<boost::intrusive_ptr<DocumentSourceMatch>, boost::intrusive_ptr<DocumentSourceMatch>>
splitMatchByModifiedFields(const boost::intrusive_ptr<DocumentSourceMatch>& match,
                           const DocumentSource::GetModPathsReturn& modifiedPathsRet) {
    // Attempt to move some or all of this $match before this stage.
    std::set<std::string> modifiedPaths;
    switch (modifiedPathsRet.type) {
        case DocumentSource::GetModPathsReturn::Type::kNotSupported:
            // We don't know what paths this stage might modify, so refrain from swapping.
            return {nullptr, match};
        case DocumentSource::GetModPathsReturn::Type::kAllPaths:
            // This stage modifies all paths, so cannot be swapped with a $match at all.
            return {nullptr, match};
        case DocumentSource::GetModPathsReturn::Type::kFiniteSet:
            modifiedPaths = std::move(modifiedPathsRet.paths);
            break;
        case DocumentSource::GetModPathsReturn::Type::kAllExcept: {
            DepsTracker depsTracker;
            match->getDependencies(&depsTracker);
            modifiedPaths = extractModifiedDependencies(depsTracker.fields, modifiedPathsRet.paths);
        }
    }
    return match->splitSourceBy(modifiedPaths);
}

}  // namespace

Pipeline::SourceContainer::iterator DocumentSource::optimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this && (std::next(itr) != container->end()));
    auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get());
    if (canSwapWithMatch() && nextMatch && !nextMatch->isTextQuery()) {
        // We're allowed to swap with a $match and the stage after us is a $match. Furthermore, the
        // $match does not contain a text search predicate, which we do not attempt to optimize
        // because such a $match must already be the first stage in the pipeline. We can attempt to
        // swap the $match or part of the $match before ourselves.
        auto splitMatch = splitMatchByModifiedFields(nextMatch, getModifiedPaths());
        invariant(splitMatch.first || splitMatch.second);

        if (splitMatch.first) {
            // At least part of the $match can be moved before this stage. Erase the original $match
            // and put the independent part before this stage. If splitMatch.second is not null,
            // then there is a new $match stage to insert after ourselves which is dependent on the
            // modified fields.
            container->erase(std::next(itr));
            container->insert(itr, std::move(splitMatch.first));
            if (splitMatch.second) {
                container->insert(std::next(itr), std::move(splitMatch.second));
            }

            // The stage before the new $match may be able to optimize further, if there is such a
            // stage.
            return std::prev(itr) == container->begin() ? std::prev(itr)
                                                        : std::prev(std::prev(itr));
        }
    }
    return doOptimizeAt(itr, container);
}

void DocumentSource::dispose() {
    if (pSource) {
        pSource->dispose();
    }
}

void DocumentSource::serializeToArray(vector<Value>& array, bool explain) const {
    Value entry = serialize(explain);
    if (!entry.missing()) {
        array.push_back(entry);
    }
}

BSONObjSet DocumentSource::allPrefixes(BSONObj obj) {
    BSONObjSet out = SimpleBSONObjComparator::kInstance.makeBSONObjSet();

    BSONObj last = {};
    for (auto&& field : obj) {
        BSONObjBuilder builder(last.objsize() + field.size());
        builder.appendElements(last);
        builder.append(field);
        last = builder.obj();
        out.insert(last);
    }

    return out;
}

BSONObjSet DocumentSource::truncateSortSet(const BSONObjSet& sorts,
                                           const std::set<std::string>& fields) {
    BSONObjSet out = SimpleBSONObjComparator::kInstance.makeBSONObjSet();

    for (auto&& sort : sorts) {
        BSONObjBuilder outputSort;

        for (auto&& key : sort) {
            auto keyName = key.fieldNameStringData();

            bool shouldAppend = true;
            for (auto&& field : fields) {
                if (keyName == field || keyName.startsWith(field + '.')) {
                    shouldAppend = false;
                    break;
                }
            }

            if (!shouldAppend) {
                break;
            }

            outputSort.append(key);
        }

        BSONObj outSortObj = outputSort.obj();
        if (!outSortObj.isEmpty()) {
            out.insert(outSortObj);
        }
    }

    return out;
}
}
