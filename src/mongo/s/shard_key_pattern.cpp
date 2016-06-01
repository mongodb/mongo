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
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/shard_key_pattern.h"

#include <vector>

#include "mongo/db/field_ref.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/hasher.h"
#include "mongo/db/index_names.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/ops/path_support.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::make_pair;
using std::pair;
using std::shared_ptr;
using std::string;
using std::unique_ptr;
using std::vector;

using pathsupport::EqualityMatches;
using mongoutils::str::stream;

const int ShardKeyPattern::kMaxShardKeySizeBytes = 512;
const unsigned int ShardKeyPattern::kMaxFlattenedInCombinations = 4000000;

Status ShardKeyPattern::checkShardKeySize(const BSONObj& shardKey) {
    if (shardKey.objsize() <= kMaxShardKeySizeBytes)
        return Status::OK();

    return Status(ErrorCodes::ShardKeyTooBig,
                  stream() << "shard keys must be less than " << kMaxShardKeySizeBytes
                           << " bytes, but key "
                           << shardKey
                           << " is "
                           << shardKey.objsize()
                           << " bytes");
}

static bool isHashedPatternEl(const BSONElement& el) {
    return el.type() == String && el.String() == IndexNames::HASHED;
}

/**
 * Currently the allowable shard keys are either
 * i) a hashed single field, e.g. { a : "hashed" }, or
 * ii) a compound list of ascending, potentially-nested field paths, e.g. { a : 1 , b.c : 1 }
 */
static vector<FieldRef*> parseShardKeyPattern(const BSONObj& keyPattern) {
    OwnedPointerVector<FieldRef> parsedPaths;
    static const vector<FieldRef*> empty;

    BSONObjIterator patternIt(keyPattern);
    while (patternIt.more()) {
        BSONElement patternEl = patternIt.next();
        parsedPaths.push_back(new FieldRef(patternEl.fieldNameStringData()));
        const FieldRef& patternPath = *parsedPaths.back();

        // Empty path
        if (patternPath.numParts() == 0)
            return empty;

        // Extra "." in path?
        if (patternPath.dottedField() != patternEl.fieldNameStringData())
            return empty;

        // Empty parts of the path, ".."?
        for (size_t i = 0; i < patternPath.numParts(); ++i) {
            if (patternPath.getPart(i).size() == 0)
                return empty;
        }

        // Numeric and ascending (1.0), or "hashed" and single field
        if (!patternEl.isNumber()) {
            if (keyPattern.nFields() != 1 || !isHashedPatternEl(patternEl))
                return empty;
        } else if (patternEl.numberInt() != 1) {
            return empty;
        }
    }

    return parsedPaths.release();
}

ShardKeyPattern::ShardKeyPattern(const BSONObj& keyPattern)
    : _keyPatternPaths(parseShardKeyPattern(keyPattern)),
      _keyPattern(_keyPatternPaths.empty() ? BSONObj() : keyPattern) {}

ShardKeyPattern::ShardKeyPattern(const KeyPattern& keyPattern)
    : _keyPatternPaths(parseShardKeyPattern(keyPattern.toBSON())),
      _keyPattern(_keyPatternPaths.empty() ? KeyPattern(BSONObj()) : keyPattern) {}

bool ShardKeyPattern::isValid() const {
    return !_keyPattern.toBSON().isEmpty();
}

bool ShardKeyPattern::isHashedPattern() const {
    return isHashedPatternEl(_keyPattern.toBSON().firstElement());
}

const KeyPattern& ShardKeyPattern::getKeyPattern() const {
    return _keyPattern;
}

const BSONObj& ShardKeyPattern::toBSON() const {
    return _keyPattern.toBSON();
}

string ShardKeyPattern::toString() const {
    return toBSON().toString();
}

static bool isShardKeyElement(const BSONElement& element, bool allowRegex) {
    // TODO: Disallow regex all the time
    if (element.eoo() || element.type() == Array || (!allowRegex && element.type() == RegEx) ||
        (element.type() == Object && !element.embeddedObject().okForStorage()))
        return false;
    return true;
}

bool ShardKeyPattern::isShardKey(const BSONObj& shardKey) const {
    // Shard keys are always of the form: { 'nested.path' : value, 'nested.path2' : value }

    if (!isValid())
        return false;

    BSONObjIterator patternIt(_keyPattern.toBSON());
    while (patternIt.more()) {
        BSONElement patternEl = patternIt.next();

        BSONElement keyEl = shardKey[patternEl.fieldNameStringData()];
        if (!isShardKeyElement(keyEl, true))
            return false;
    }

    return true;
}

BSONObj ShardKeyPattern::normalizeShardKey(const BSONObj& shardKey) const {
    // Shard keys are always of the form: { 'nested.path' : value, 'nested.path2' : value }
    // and in the same order as the key pattern

    if (!isValid())
        return BSONObj();

    // We want to return an empty key if users pass us something that's not a shard key
    if (shardKey.nFields() > _keyPattern.toBSON().nFields())
        return BSONObj();

    BSONObjBuilder keyBuilder;
    BSONObjIterator patternIt(_keyPattern.toBSON());
    while (patternIt.more()) {
        BSONElement patternEl = patternIt.next();

        BSONElement keyEl = shardKey[patternEl.fieldNameStringData()];

        if (!isShardKeyElement(keyEl, true))
            return BSONObj();

        keyBuilder.appendAs(keyEl, patternEl.fieldName());
    }

    dassert(isShardKey(keyBuilder.asTempObj()));
    return keyBuilder.obj();
}

static BSONElement extractKeyElementFromMatchable(const MatchableDocument& matchable,
                                                  StringData pathStr) {
    ElementPath path;
    path.init(pathStr);
    path.setTraverseNonleafArrays(false);
    path.setTraverseLeafArray(false);

    MatchableDocument::IteratorHolder matchIt(&matchable, &path);
    if (!matchIt->more())
        return BSONElement();

    BSONElement matchEl = matchIt->next().element();
    // We shouldn't have more than one element - we don't expand arrays
    dassert(!matchIt->more());

    return matchEl;
}

BSONObj ShardKeyPattern::extractShardKeyFromMatchable(const MatchableDocument& matchable) const {
    if (!isValid())
        return BSONObj();

    BSONObjBuilder keyBuilder;

    BSONObjIterator patternIt(_keyPattern.toBSON());
    while (patternIt.more()) {
        BSONElement patternEl = patternIt.next();
        BSONElement matchEl =
            extractKeyElementFromMatchable(matchable, patternEl.fieldNameStringData());

        if (!isShardKeyElement(matchEl, true))
            return BSONObj();

        if (isHashedPatternEl(patternEl)) {
            keyBuilder.append(
                patternEl.fieldName(),
                BSONElementHasher::hash64(matchEl, BSONElementHasher::DEFAULT_HASH_SEED));
        } else {
            // NOTE: The matched element may *not* have the same field name as the path -
            // index keys don't contain field names, for example
            keyBuilder.appendAs(matchEl, patternEl.fieldName());
        }
    }

    dassert(isShardKey(keyBuilder.asTempObj()));
    return keyBuilder.obj();
}

BSONObj ShardKeyPattern::extractShardKeyFromDoc(const BSONObj& doc) const {
    BSONMatchableDocument matchable(doc);
    return extractShardKeyFromMatchable(matchable);
}

static BSONElement findEqualityElement(const EqualityMatches& equalities, const FieldRef& path) {
    int parentPathPart;
    const BSONElement parentEl =
        pathsupport::findParentEqualityElement(equalities, path, &parentPathPart);

    if (parentPathPart == static_cast<int>(path.numParts()))
        return parentEl;

    if (parentEl.type() != Object)
        return BSONElement();

    StringData suffixStr = path.dottedSubstring(parentPathPart, path.numParts());
    BSONMatchableDocument matchable(parentEl.Obj());
    return extractKeyElementFromMatchable(matchable, suffixStr);
}

StatusWith<BSONObj> ShardKeyPattern::extractShardKeyFromQuery(OperationContext* txn,
                                                              const BSONObj& basicQuery) const {
    if (!isValid())
        return StatusWith<BSONObj>(BSONObj());

    auto qr = stdx::make_unique<QueryRequest>(NamespaceString(""));
    qr->setFilter(basicQuery);

    auto statusWithCQ = CanonicalQuery::canonicalize(txn, std::move(qr), ExtensionsCallbackNoop());
    if (!statusWithCQ.isOK()) {
        return StatusWith<BSONObj>(statusWithCQ.getStatus());
    }
    unique_ptr<CanonicalQuery> query = std::move(statusWithCQ.getValue());

    return extractShardKeyFromQuery(*query);
}

StatusWith<BSONObj> ShardKeyPattern::extractShardKeyFromQuery(const CanonicalQuery& query) const {
    if (!isValid())
        return StatusWith<BSONObj>(BSONObj());

    // Extract equalities from query.
    EqualityMatches equalities;
    // TODO: Build the path set initially?
    FieldRefSet keyPatternPathSet(_keyPatternPaths.vector());
    // We only care about extracting the full key pattern paths - if they don't exist (or are
    // conflicting), we don't contain the shard key.
    Status eqStatus =
        pathsupport::extractFullEqualityMatches(*query.root(), keyPatternPathSet, &equalities);
    // NOTE: Failure to extract equality matches just means we return no shard key - it's not
    // an error we propagate
    if (!eqStatus.isOK())
        return StatusWith<BSONObj>(BSONObj());

    // Extract key from equalities
    // NOTE: The method below is equivalent to constructing a BSONObj and running
    // extractShardKeyFromMatchable, but doesn't require creating the doc.

    BSONObjBuilder keyBuilder;
    // Iterate the parsed paths to avoid re-parsing
    for (OwnedPointerVector<FieldRef>::const_iterator it = _keyPatternPaths.begin();
         it != _keyPatternPaths.end();
         ++it) {
        const FieldRef& patternPath = **it;
        BSONElement equalEl = findEqualityElement(equalities, patternPath);

        if (!isShardKeyElement(equalEl, false))
            return StatusWith<BSONObj>(BSONObj());

        if (isHashedPattern()) {
            keyBuilder.append(
                patternPath.dottedField(),
                BSONElementHasher::hash64(equalEl, BSONElementHasher::DEFAULT_HASH_SEED));
        } else {
            // NOTE: The equal element may *not* have the same field name as the path -
            // nested $and, $eq, for example
            keyBuilder.appendAs(equalEl, patternPath.dottedField());
        }
    }

    dassert(isShardKey(keyBuilder.asTempObj()));
    return StatusWith<BSONObj>(keyBuilder.obj());
}

bool ShardKeyPattern::isUniqueIndexCompatible(const BSONObj& uniqueIndexPattern) const {
    dassert(!KeyPattern::isHashedKeyPattern(uniqueIndexPattern));

    if (!uniqueIndexPattern.isEmpty() &&
        string("_id") == uniqueIndexPattern.firstElementFieldName()) {
        return true;
    }

    return _keyPattern.toBSON().isFieldNamePrefixOf(uniqueIndexPattern);
}

BoundList ShardKeyPattern::flattenBounds(const IndexBounds& indexBounds) const {
    invariant(indexBounds.fields.size() == (size_t)_keyPattern.toBSON().nFields());

    // If any field is unsatisfied, return empty bound list.
    for (vector<OrderedIntervalList>::const_iterator it = indexBounds.fields.begin();
         it != indexBounds.fields.end();
         it++) {
        if (it->intervals.size() == 0) {
            return BoundList();
        }
    }
    // To construct our bounds we will generate intervals based on bounds for
    // the first field, then compound intervals based on constraints for the first
    // 2 fields, then compound intervals for the first 3 fields, etc.
    // As we loop through the fields, we start generating new intervals that will later
    // get extended in another iteration of the loop.  We define these partially constructed
    // intervals using pairs of BSONObjBuilders (shared_ptrs, since after one iteration of the
    // loop they still must exist outside their scope).
    typedef vector<pair<shared_ptr<BSONObjBuilder>, shared_ptr<BSONObjBuilder>>> BoundBuilders;

    BoundBuilders builders;
    builders.push_back(make_pair(shared_ptr<BSONObjBuilder>(new BSONObjBuilder()),
                                 shared_ptr<BSONObjBuilder>(new BSONObjBuilder())));
    BSONObjIterator keyIter(_keyPattern.toBSON());
    // until equalityOnly is false, we are just dealing with equality (no range or $in queries).
    bool equalityOnly = true;

    for (size_t i = 0; i < indexBounds.fields.size(); i++) {
        BSONElement e = keyIter.next();

        StringData fieldName = e.fieldNameStringData();

        // get the relevant intervals for this field, but we may have to transform the
        // list of what's relevant according to the expression for this field
        const OrderedIntervalList& oil = indexBounds.fields[i];
        const vector<Interval>& intervals = oil.intervals;

        if (equalityOnly) {
            if (intervals.size() == 1 && intervals.front().isPoint()) {
                // this field is only a single point-interval
                BoundBuilders::const_iterator j;
                for (j = builders.begin(); j != builders.end(); ++j) {
                    j->first->appendAs(intervals.front().start, fieldName);
                    j->second->appendAs(intervals.front().end, fieldName);
                }
            } else {
                // This clause is the first to generate more than a single point.
                // We only execute this clause once. After that, we simplify the bound
                // extensions to prevent combinatorial explosion.
                equalityOnly = false;

                BoundBuilders newBuilders;

                for (BoundBuilders::const_iterator it = builders.begin(); it != builders.end();
                     ++it) {
                    BSONObj first = it->first->obj();
                    BSONObj second = it->second->obj();

                    for (vector<Interval>::const_iterator interval = intervals.begin();
                         interval != intervals.end();
                         ++interval) {
                        uassert(17439,
                                "combinatorial limit of $in partitioning of results exceeded",
                                newBuilders.size() < kMaxFlattenedInCombinations);
                        newBuilders.push_back(  //
                            make_pair(shared_ptr<BSONObjBuilder>(new BSONObjBuilder()),
                                      shared_ptr<BSONObjBuilder>(new BSONObjBuilder())));
                        newBuilders.back().first->appendElements(first);
                        newBuilders.back().second->appendElements(second);
                        newBuilders.back().first->appendAs(interval->start, fieldName);
                        newBuilders.back().second->appendAs(interval->end, fieldName);
                    }
                }
                builders = newBuilders;
            }
        } else {
            // if we've already generated a range or multiple point-intervals
            // just extend what we've generated with min/max bounds for this field
            BoundBuilders::const_iterator j;
            for (j = builders.begin(); j != builders.end(); ++j) {
                j->first->appendAs(intervals.front().start, fieldName);
                j->second->appendAs(intervals.back().end, fieldName);
            }
        }
    }
    BoundList ret;
    for (BoundBuilders::const_iterator i = builders.begin(); i != builders.end(); ++i)
        ret.push_back(make_pair(i->first->obj(), i->second->obj()));
    return ret;
}

}  // namespace mongo
