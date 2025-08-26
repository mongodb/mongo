/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/container_size_helper.h"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <set>
#include <string>
#include <utility>

#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>

namespace mongo {
class CollatorInterface;
class MatchExpression;

class IndexPathProjection;
using WildcardProjection = IndexPathProjection;

/**
 * A CoreIndexInfo is a representation of an index in the catalog with parsed information which is
 * used for updating indexability discriminators. Its lifetime is not tied to the underlying
 * collection. It is a subset of IndexEntry and is missing fields that are expensive or unavailable.
 */
struct CoreIndexInfo {

    struct Identifier;

    CoreIndexInfo(const BSONObj& kp,
                  IndexType type,
                  bool sp,
                  Identifier ident,
                  const MatchExpression* fe = nullptr,
                  const CollatorInterface* ci = nullptr,
                  const IndexPathProjection* indexPathProj = nullptr,
                  std::shared_ptr<const IndexCatalogEntry> iceStorage = nullptr)
        : identifier(std::move(ident)),
          keyPattern(kp),
          filterExpr(fe),
          type(type),
          sparse(sp),
          collator(ci),
          indexPathProjection(indexPathProj),
          indexCatalogEntryStorage(std::move(iceStorage)) {
        // If a projection executor exists, we always expect a $** index
        if (indexPathProjection != nullptr)
            invariant(type == IndexType::INDEX_WILDCARD);
    }

    virtual ~CoreIndexInfo() = default;

    /**
     * This struct is used to uniquely identify an index. The index "Identifier" has two
     * components: catalog name, and "disambiguator". The catalog name is just the name of the
     * index in the catalog. The disambiguator is used by the planner when multiple IndexEntries
     * may refer to the same underlying index in the catalog. This can only happen with $**
     * indices. Otherwise, the disambiguator should be empty.
     *
     * Has the same comparison and equality semantics as std::pair<string, string>.
     *
     */
    struct Identifier {
        explicit Identifier(std::string aCatalogName) : catalogName(std::move(aCatalogName)) {}

        Identifier(std::string aCatalogName, std::string nameDisambiguator)
            : catalogName(std::move(aCatalogName)), disambiguator(std::move(nameDisambiguator)) {}

        bool operator==(const Identifier& other) const {
            return other.catalogName == catalogName && other.disambiguator == disambiguator;
        }

        bool operator!=(const Identifier& other) const {
            return !(*this == other);
        }

        bool operator<(const Identifier& other) const {
            const auto cmpRes = catalogName.compare(other.catalogName);
            if (cmpRes != 0) {
                return cmpRes < 0;
            }
            return disambiguator < other.disambiguator;
        }

        std::string toString() const {
            return "(" + catalogName + ", " + disambiguator + ")";
        }

        uint64_t estimateObjectSizeInBytes() const {
            return catalogName.capacity() + disambiguator.capacity() + sizeof(*this);
        }
        // The name of the index in the catalog.
        std::string catalogName;

        // A string used for disambiguating multiple IndexEntries with the same catalogName (such
        // as in the case with a wildcard index).
        std::string disambiguator;
    } identifier;

    // Describes the keys of this index. Each BSONElement in 'keyPattern' describes one key part.
    // Its name is the path to the field indexed by this part, and its value is the type of indexing
    // done on that field, e.g. double 1.0/-1.0 for ascending/descending or string "hashed" for
    // hashing.
    BSONObj keyPattern;

    // If this index is a partial index, 'filterExpr' is the MatchExpression representing the
    // filter predicate. Otherwise, 'filterExpr' is null. It is the caller's responsibility to
    // ensure that the pointer remains valid for the lifetime of this CoreIndexInfo.
    // See 'indexCatalogEntryStorage' for more details.
    const MatchExpression* filterExpr;

    // What type of index is this? (What access method can we use on the index described by the
    // keyPattern?)
    IndexType type;

    bool sparse;

    // Null if this index orders strings according to the simple binary compare. If non-null,
    // represents the collator used to generate index keys for indexed strings.
    // It is the caller's responsibility to ensure that the pointer remains valid for the lifetime
    // of this CoreIndexInfo.
    // See 'indexCatalogEntryStorage' for more details.
    const CollatorInterface* collator = nullptr;

    // For $** indexes, a pointer to the projection executor owned by the index access method. Null
    // unless this IndexEntry represents a wildcard index, in which case this is always non-null.
    // It is the caller's responsibility to ensure that the pointer remains valid for the lifetime
    // of this CoreIndexInfo.
    // See 'indexCatalogEntryStorage' for more details.
    const IndexPathProjection* indexPathProjection = nullptr;

    // Optional shared_ptr to the IndexCatalogEntry (storage's in-memory representation of an index
    // in the catalog). This is used to keep the IndexCatalogEntry alive as long as this
    // CoreIndexInfo. This is useful because the 'filterExpr', 'collator' and 'indexPathProjection'
    // members are often pointers to data owned by IndexCatalogEntry.
    std::shared_ptr<const IndexCatalogEntry> indexCatalogEntryStorage;
};

/**
 * An IndexEntry is a representation of an index in the catalog with parsed information which is
 * helpful for query planning. Its lifetime is not tied to the underlying collection. In contrast
 * to CoreIndexInfo, it includes information such as 'multikeyPaths' which can require resources to
 * compute (i.e. for wildcard indexes, this requires reading the index) and so may not always be
 * available.
 */
struct IndexEntry : CoreIndexInfo {
    IndexEntry(const BSONObj& kp,
               IndexType type,
               IndexDescriptor::IndexVersion version,
               bool mk,
               MultikeyPaths mkp,
               std::set<FieldRef> multikeyPathSet,
               bool sp,
               bool unq,
               Identifier ident,
               const MatchExpression* fe,
               const BSONObj& io,
               const CollatorInterface* ci,
               const WildcardProjection* wildcardProjection,
               std::shared_ptr<const IndexCatalogEntry> iceStorage = nullptr,
               size_t wildcardPos = 0)
        : CoreIndexInfo(
              kp, type, sp, std::move(ident), fe, ci, wildcardProjection, std::move(iceStorage)),
          version(version),
          multikey(mk),
          unique(unq),
          multikeyPaths(std::move(mkp)),
          multikeyPathSet(std::move(multikeyPathSet)),
          infoObj(io),
          wildcardFieldPos(wildcardPos) {
        // The caller must not supply multikey metadata in two different formats.
        invariant(this->multikeyPaths.empty() || this->multikeyPathSet.empty());
    }

    IndexEntry(const IndexEntry&) = default;
    IndexEntry(IndexEntry&&) = default;

    IndexEntry& operator=(const IndexEntry&) = default;
    IndexEntry& operator=(IndexEntry&&) = default;

    ~IndexEntry() override {
        // An IndexEntry should never have both formats of multikey metadata simultaneously.
        invariant(multikeyPaths.empty() || multikeyPathSet.empty());
    }

    /**
     * Returns true if 'indexedField' has any multikey components. For example, returns true if this
     * index has a multikey component "a", and 'indexedField' is "a.b". Illegal to call unless
     * 'indexedField' is present in this index's key pattern.
     *
     * For indexes created on older versions we may not have path-level multikey information. In
     * these cases we only have a single boolean to track whether any path in the index is multikey.
     * If this is the case we defensively return true for any path.
     */
    bool pathHasMultikeyComponent(StringData indexedField) const;

    /**
     * It's not obvious which element is the wildcard element in a compound wildcard index key
     * pattern. This helper can give you the wildcard element based on the position tracked in
     * 'IndexEntry'. Only valid to call on a WILDCARD index.
     */
    BSONElement getWildcardField() const;

    bool operator==(const IndexEntry& rhs) const {
        // Indexes are logically equal when names are equal.
        return this->identifier == rhs.identifier;
    }

    std::string toString() const;

    uint64_t estimateObjectSizeInBytes() const {

        return  // For each element in 'multikeyPaths' add the 'length of the vector * size of the
                // vector element'.
            container_size_helper::estimateObjectSizeInBytes(
                multikeyPaths,
                [](const auto& keyPath) {
                    // Calculate the size of each std::set in 'multiKeyPaths'.
                    return container_size_helper::estimateObjectSizeInBytes(keyPath);
                },
                true) +
            container_size_helper::estimateObjectSizeInBytes(
                multikeyPathSet,
                [](const auto& fieldRef) { return fieldRef.estimateObjectSizeInBytes(); },
                false) +
            // Subtract static size of 'identifier' since it is already included in
            // 'sizeof(*this)'.
            (identifier.estimateObjectSizeInBytes() - sizeof(identifier)) +
            // Add the runtime BSONObj size of 'keyPattern'.
            keyPattern.objsize() +
            // The BSON size of the 'infoObj' is purposefully excluded since its ownership is shared
            // with the index catalog.
            // Add size of the object.
            sizeof(*this);
    }

    IndexDescriptor::IndexVersion version;
    bool multikey;
    bool unique;

    // If non-empty, 'multikeyPaths' is a vector with size equal to the number of elements in the
    // index key pattern. Each element in the vector is an ordered set of positions (starting at 0)
    // into the corresponding indexed field that represent what prefixes of the indexed field cause
    // the index to be multikey.
    //
    // An IndexEntry may either represent multikey metadata as a fixed-size MultikeyPaths vector, or
    // as an arbitrarily large set of field refs, but not both. That is, either 'multikeyPaths' or
    // 'multikeyPathSet' must be empty.
    MultikeyPaths multikeyPaths;

    // A set of multikey paths. Used instead of 'multikeyPaths' when there could be arbitrarily many
    // multikey paths associated with this index entry.
    //
    // An IndexEntry may either represent multikey metadata as a fixed-size MultikeyPaths vector, or
    // as an arbitrarily large set of field refs, but not both. That is, either 'multikeyPaths' or
    // 'multikeyPathSet' must be empty.
    std::set<FieldRef> multikeyPathSet;

    // Geo indices have extra parameters.  We need those available to plan correctly.
    BSONObj infoObj;

    // Position of the replaced wildcard index field in the keyPattern, applied to Wildcard Indexes
    // only.
    size_t wildcardFieldPos;
};

template <typename H>
H AbslHashValue(H h, const IndexEntry::Identifier& identifier) {
    return H::combine(std::move(h), identifier.catalogName, identifier.disambiguator);
}

std::ostream& operator<<(std::ostream& stream, const IndexEntry::Identifier& ident);
StringBuilder& operator<<(StringBuilder& builder, const IndexEntry::Identifier& ident);
}  // namespace mongo
