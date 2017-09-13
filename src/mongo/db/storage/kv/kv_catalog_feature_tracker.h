/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#pragma once

#include <cstdint>
#include <memory>
#include <type_traits>

#include "mongo/db/storage/kv/kv_catalog.h"

namespace mongo {

class OperationContext;
class RecordId;
class RecordStore;

/**
 * Manages the contents of a document in the KVCatalog used to restrict downgrade compatibility.
 *
 * When a new feature is enabled on a collection or index in the data files, a bit is set in one of
 * the fields of the document. Older versions won't recognize this bit and will fail to start up as
 * a result.
 *
 * The inserted document serves a similar purpose to the DataFileVersion class used with the MMAPv1
 * storage engine.
 */
class KVCatalog::FeatureTracker {
public:
    /**
     * Bit flags representing whether a particular feature is enabled on a least one collection or
     * index in the data files. Features included in this enumeration always require user
     * intervention on downgrade.
     *
     * The next feature added to this enumeration should use the current value of 'kNextFeatureBit',
     * and 'kNextFeatureBit' should be changed to the next largest power of two.
     */
    enum class NonRepairableFeature : std::uint64_t {
        kCollation = 1 << 0,
        kNextFeatureBit = 1 << 1
    };

    using NonRepairableFeatureMask = std::underlying_type<NonRepairableFeature>::type;

    /**
     * Bit flags representing whether a particular feature is enabled on a least one collection or
     * index in the data files. Features included in this enumeration either (a) don't require user
     * intervention on downgrade, or (b) are no longer enabled if --repair is done with an older
     * version.
     *
     * The next feature added to this enumeration should use the current value of 'kNextFeatureBit',
     * and 'kNextFeatureBit' should be changed to the next largest power of two.
     */
    enum class RepairableFeature : std::uint64_t {
        kPathLevelMultikeyTracking = 1 << 0,
        kNextFeatureBit = 1 << 1
    };

    using RepairableFeatureMask = std::underlying_type<RepairableFeature>::type;

    /**
     * Returns true if 'obj' represents the contents of the feature document that was previously
     * inserted into the KVCatalog, and returns false otherwise.
     *
     * This function should return true for at most one document in the KVCatalog.
     */
    static bool isFeatureDocument(BSONObj obj);

    /**
     * Returns a FeatureTracker instance to manage the contents of the feature document located at
     * 'rid' in the record store 'catalog->_rs'.
     *
     * It is invalid to call this function when isFeatureDocument() returns false for the record
     * data associated with 'rid'.
     */
    static std::unique_ptr<FeatureTracker> get(OperationContext* opCtx,
                                               KVCatalog* catalog,
                                               RecordId rid);

    /**
     * Returns a FeatureTracker instance to manage the contents of a feature document. The feature
     * document isn't inserted into 'rs' as a result of calling this function. Instead, the feature
     * document is inserted into 'rs' when putInfo() is first called.
     *
     * It is invalid to call this function when isFeatureDocument() returns true for some document
     * in the record store 'catalog->_rs'.
     */
    static std::unique_ptr<FeatureTracker> create(OperationContext* opCtx, KVCatalog* catalog);

    /**
     * Returns whethers the data files are compatible with the current code:
     *
     *   - Status::OK() if the data files are compatible with the current code.
     *
     *   - ErrorCodes::CanRepairToDowngrade if the data files are incompatible with the current
     *     code, but a --repair would make them compatible. For example, when rebuilding all indexes
     *     in the data files would resolve the incompatibility.
     *
     *   - ErrorCodes::MustUpgrade if the data files are incompatible with the current code and a
     *     newer version is required to start up.
     */
    Status isCompatibleWithCurrentCode(OperationContext* opCtx) const;

    /**
     * Returns true if 'feature' is tracked in the document, and returns false otherwise.
     */
    bool isNonRepairableFeatureInUse(OperationContext* opCtx, NonRepairableFeature feature) const;

    /**
     * Sets the specified non-repairable feature as being enabled on at least one collection or
     * index in the data files.
     */
    void markNonRepairableFeatureAsInUse(OperationContext* opCtx, NonRepairableFeature feature);

    /**
     * Sets the specified non-repairable feature as not being enabled on any collection or index in
     * the data files.
     */
    void markNonRepairableFeatureAsNotInUse(OperationContext* opCtx, NonRepairableFeature feature);

    /**
     * Returns true if 'feature' is tracked in the document, and returns false otherwise.
     */
    bool isRepairableFeatureInUse(OperationContext* opCtx, RepairableFeature feature) const;

    /**
     * Sets the specified repairable feature as being enabled on at least one collection or index in
     * the data files.
     */
    void markRepairableFeatureAsInUse(OperationContext* opCtx, RepairableFeature feature);

    /**
     * Sets the specified repairable feature as not being enabled on any collection or index in the
     * data files.
     */
    void markRepairableFeatureAsNotInUse(OperationContext* opCtx, RepairableFeature feature);

    void setUsedNonRepairableFeaturesMaskForTestingOnly(NonRepairableFeatureMask mask) {
        _usedNonRepairableFeaturesMask = mask;
    }

    void setUsedRepairableFeaturesMaskForTestingOnly(RepairableFeatureMask mask) {
        _usedRepairableFeaturesMask = mask;
    }

private:
    // Must go through FeatureTracker::get() or FeatureTracker::create().
    FeatureTracker(KVCatalog* catalog, RecordId rid) : _catalog(catalog), _rid(rid) {}

    struct FeatureBits {
        NonRepairableFeatureMask nonRepairableFeatures;
        RepairableFeatureMask repairableFeatures;
    };

    FeatureBits getInfo(OperationContext* opCtx) const;

    void putInfo(OperationContext* opCtx, const FeatureBits& versionInfo);

    KVCatalog* _catalog;
    RecordId _rid;

    NonRepairableFeatureMask _usedNonRepairableFeaturesMask =
        static_cast<NonRepairableFeatureMask>(NonRepairableFeature::kNextFeatureBit) - 1;

    RepairableFeatureMask _usedRepairableFeaturesMask =
        static_cast<RepairableFeatureMask>(RepairableFeature::kNextFeatureBit) - 1;
};

}  // namespace mongo
