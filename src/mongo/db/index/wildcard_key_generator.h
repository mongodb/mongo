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

#include "mongo/db/exec/projection_exec_agg.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"

namespace mongo {

/**
 * This class is responsible for generating an aggregation projection based on the keyPattern and
 * pathProjection specs, and for subsequently extracting the set of all path-value pairs for each
 * document.
 */
class WildcardKeyGenerator {
public:
    static constexpr StringData kSubtreeSuffix = ".$**"_sd;

    /**
     * Returns an owned ProjectionExecAgg identical to the one that WildcardKeyGenerator will use
     * internally when generating the keys for the $** index, as defined by the 'keyPattern' and
     * 'pathProjection' arguments.
     */
    static std::unique_ptr<ProjectionExecAgg> createProjectionExec(BSONObj keyPattern,
                                                                   BSONObj pathProjection);

    WildcardKeyGenerator(BSONObj keyPattern,
                         BSONObj pathProjection,
                         const CollatorInterface* collator,
                         KeyString::Version keyStringVersion,
                         Ordering ordering);

    /**
     * Returns a pointer to the key generator's underlying ProjectionExecAgg.
     */
    const ProjectionExecAgg* getProjectionExec() const {
        return _projExec.get();
    }

    /**
     * Applies the appropriate Wildcard projection to the input doc, and then adds one key-value
     * pair to the set 'keys' for each leaf node in the post-projection document:
     *      { '': 'path.to.field', '': <collation-aware-field-value> }
     * Also adds one entry to 'multikeyPaths' for each array encountered in the post-projection
     * document, in the following format:
     *      { '': 1, '': 'path.to.array' }
     */
    void generateKeys(BSONObj inputDoc,
                      KeyStringSet* keys,
                      KeyStringSet* multikeyPaths,
                      boost::optional<RecordId> id = boost::none) const;

private:
    // Traverses every path of the post-projection document, adding keys to the set as it goes.
    void _traverseWildcard(BSONObj obj,
                           bool objIsArray,
                           FieldRef* path,
                           KeyStringSet* keys,
                           KeyStringSet* multikeyPaths,
                           boost::optional<RecordId> id) const;

    // Helper functions to format the entry appropriately before adding it to the key/path tracker.
    void _addMultiKey(const FieldRef& fullPath, KeyStringSet* multikeyPaths) const;
    void _addKey(BSONElement elem,
                 const FieldRef& fullPath,
                 KeyStringSet* keys,
                 boost::optional<RecordId> id) const;

    // Helper to check whether the element is a nested array, and conditionally add it to 'keys'.
    bool _addKeyForNestedArray(BSONElement elem,
                               const FieldRef& fullPath,
                               bool enclosingObjIsArray,
                               KeyStringSet* keys,
                               boost::optional<RecordId> id) const;
    bool _addKeyForEmptyLeaf(BSONElement elem,
                             const FieldRef& fullPath,
                             KeyStringSet* keys,
                             boost::optional<RecordId> id) const;

    std::unique_ptr<ProjectionExecAgg> _projExec;
    const CollatorInterface* _collator;
    const BSONObj _keyPattern;
    const KeyString::Version _keyStringVersion;
    const Ordering _ordering;
};
}  // namespace mongo
