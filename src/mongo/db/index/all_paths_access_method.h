/**
 *    Copyright (C) 2018 MongoDB, Inc.
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

#include "mongo/db/index/all_paths_key_generator.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/jsobj.h"

namespace mongo {

/**
 * Class which is responsible for generating and providing access to AllPaths index keys. Any index
 * created with { "$**": ±1 } or { "path.$**": ±1 } uses this class.
 */
class AllPathsAccessMethod : public IndexAccessMethod {
public:
    AllPathsAccessMethod(IndexCatalogEntry* allPathsState, SortedDataInterface* btree);

    /**
     * Returns 'true' if the index should become multikey on the basis of the passed arguments.
     * Because it is possible for a $** index to generate multiple keys per document without any of
     * them lying along a multikey (i.e. array) path, this method will only return 'true' if one or
     * more multikey metadata keys have been generated; that is, if the 'multikeyMetadataKeys'
     * BSONObjSet is non-empty.
     */
    bool shouldMarkIndexAsMultikey(const BSONObjSet& keys,
                                   const BSONObjSet& multikeyMetadataKeys,
                                   const MultikeyPaths& multikeyPaths) const final;

private:
    void doGetKeys(const BSONObj& obj,
                   BSONObjSet* keys,
                   BSONObjSet* multikeyMetadataKeys,
                   MultikeyPaths* multikeyPaths) const final;

    const AllPathsKeyGenerator _keyGen;
};
}  // namespace mongo
