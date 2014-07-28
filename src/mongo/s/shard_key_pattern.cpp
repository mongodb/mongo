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

#include "mongo/s/shard_key_pattern.h"

#include "mongo/db/keypattern.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    bool isUniqueIndexCompatible(const BSONObj& shardKeyPattern,
                                 const BSONObj& uIndexKeyPattern) {

        if ( !uIndexKeyPattern.isEmpty()
             && string( "_id" ) == uIndexKeyPattern.firstElementFieldName() ) {
            return true;
        }

        return shardKeyPattern.isFieldNamePrefixOf( uIndexKeyPattern );
    }

    bool isShardDocSizeValid(const BSONObj& shardKey, const BSONObj& doc, string* errMsg) {
        string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        const KeyPattern shardKeyPattern(shardKey);
        BSONObj key = shardKeyPattern.extractSingleKey(doc);

        if (key.objsize() > kMaxShardKeySize) {
            *errMsg = str::stream() << "shard key is larger than " << kMaxShardKeySize
                                    << " bytes: " << key.objsize();
            return false;
        }

        return true;
    }
}
