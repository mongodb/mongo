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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <string>

#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index_names.h"
#include "mongo/db/jsobj.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

class CollatorInterface;
class MatchExpression;

/**
 * This name sucks, but every name involving 'index' is used somewhere.
 */
struct IndexEntry {
    /**
     * Use this constructor if you're making an IndexEntry from the catalog.
     */
    IndexEntry(const BSONObj& kp,
               const std::string& accessMethod,
               bool mk,
               const MultikeyPaths& mkp,
               bool sp,
               bool unq,
               const std::string& n,
               const MatchExpression* fe,
               const BSONObj& io,
               const CollatorInterface* ci)
        : keyPattern(kp),
          multikey(mk),
          multikeyPaths(mkp),
          sparse(sp),
          unique(unq),
          name(n),
          filterExpr(fe),
          infoObj(io),
          collator(ci) {
        type = IndexNames::nameToType(accessMethod);
    }

    /**
     * For testing purposes only.
     */
    IndexEntry(const BSONObj& kp,
               bool mk,
               bool sp,
               bool unq,
               const std::string& n,
               const MatchExpression* fe,
               const BSONObj& io)
        : keyPattern(kp),
          multikey(mk),
          sparse(sp),
          unique(unq),
          name(n),
          filterExpr(fe),
          infoObj(io) {
        type = IndexNames::nameToType(IndexNames::findPluginName(keyPattern));
    }

    /**
     * For testing purposes only.
     */
    IndexEntry(const BSONObj& kp)
        : keyPattern(kp),
          multikey(false),
          sparse(false),
          unique(false),
          name("test_foo"),
          filterExpr(nullptr),
          infoObj(BSONObj()) {
        type = IndexNames::nameToType(IndexNames::findPluginName(keyPattern));
    }

    std::string toString() const;

    BSONObj keyPattern;

    bool multikey;

    // If non-empty, 'multikeyPaths' is a vector with size equal to the number of elements in the
    // index key pattern. Each element in the vector is an ordered set of positions (starting at 0)
    // into the corresponding indexed field that represent what prefixes of the indexed field cause
    // the index to be multikey.
    MultikeyPaths multikeyPaths;

    bool sparse;

    bool unique;

    std::string name;

    const MatchExpression* filterExpr;

    // Geo indices have extra parameters.  We need those available to plan correctly.
    BSONObj infoObj;

    // What type of index is this?  (What access method can we use on the index described
    // by the keyPattern?)
    IndexType type;

    // Null if this index orders strings according to the simple binary compare. If non-null,
    // represents the collator used to generate index keys for indexed strings.
    const CollatorInterface* collator = nullptr;
};

}  // namespace mongo
