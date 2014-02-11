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

#include "mongo/db/index_names.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    /**
     * We need to know what 'type' an index is in order to plan correctly.
     * Rather than look this up repeatedly we figure it out once.
     */
    enum IndexType {
        INDEX_BTREE,
        INDEX_2D,
        INDEX_HAYSTACK,
        INDEX_2DSPHERE,
        INDEX_TEXT,
        INDEX_HASHED,
    };

    /**
     * This name sucks, but every name involving 'index' is used somewhere.
     */
    struct IndexEntry {
        IndexEntry(const BSONObj& kp,
                   bool mk = false,
                   bool sp = false,
                   const string& n = "default_name",
                   const BSONObj& io = BSONObj())
            : keyPattern(kp),
              multikey(mk),
              sparse(sp),
              name(n),
              infoObj(io) {

            // XXX: This is the wrong way to set this and this is dangerous.  We need to check in
            // the catalog to see if we should override the plugin name instead of just grabbing it
            // directly from the key pattern.  Move this a level higher to wherever IndexEntry is
            // created.
            //
            // An example of the Bad Thing That We Must Avoid:
            // 1. Create a 2dsphere index in 2.4, insert some docs.
            // 2. Downgrade to 2.2.  Insert some more docs into the collection w/the 2dsphere
            //    index.  2.2 treats the index as a normal btree index and creates keys accordingly.
            // 3. Using the 2dsphere index in 2.4 gives wrong results or assert-fails or crashes as
            //    the data isn't what we expect.
            string typeStr = IndexNames::findPluginName(keyPattern);

            if (IndexNames::GEO_2D == typeStr) {
                type = INDEX_2D;
            }
            else if (IndexNames::GEO_HAYSTACK == typeStr) {
                type = INDEX_HAYSTACK;
            }
            else if (IndexNames::GEO_2DSPHERE == typeStr) {
                type = INDEX_2DSPHERE;
            }
            else if (IndexNames::TEXT == typeStr) {
                type = INDEX_TEXT;
            }
            else if (IndexNames::HASHED == typeStr) {
                type = INDEX_HASHED;
            }
            else {
                type = INDEX_BTREE;
            }
        }

        BSONObj keyPattern;

        bool multikey;

        bool sparse;

        string name;

        // Geo indices have extra parameters.  We need those available to plan correctly.
        BSONObj infoObj;

        // What type of index is this?  (What access method can we use on the index described
        // by the keyPattern?)
        IndexType type;

        std::string toString() const {
            mongoutils::str::stream ss;
            ss << "kp: "  << keyPattern.toString();

            if (multikey) {
                ss << " multikey";
            }

            if (sparse) {
                ss << " sparse";
            }

            if (!infoObj.isEmpty()) {
                ss << " io: " << infoObj.toString();
            }

            return ss;
        }
    };

}  // namespace mongo
