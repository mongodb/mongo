/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/bson/ordering.h"
#include "mongo/db/diskloc.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/structure/head_manager.h"
#include "mongo/db/structure/record_store.h"

#pragma once

namespace mongo {
namespace transition {

    /**
     * This is the interface for interacting with the Btree.  The index access and catalog layers
     * should use this.
     */
    class BtreeInterface {
    public:
        virtual ~BtreeInterface() { }

        /**
         * Represents a location in the Btree.
         */
        struct BtreeLocation {
            // XXX: can these be private?
            DiskLoc bucket;
            int pos;
        };

        /**
         * Interact with the Btree through the BtreeInterface.
         *
         * Does not own headManager.
         * Does not own recordStore.
         * Copies ordering.
         */
        static BtreeInterface* getInterface(HeadManager* headManager,
                                            RecordStore* recordStore,
                                            const Ordering& ordering,
                                            int version);

        virtual Status insert(const BSONObj& key, const DiskLoc& loc, bool dupsAllowed) = 0;

        virtual bool unindex(const BSONObj& key, const DiskLoc& loc) = 0;

        virtual bool locate(const BSONObj& key,
                            const DiskLoc& loc,
                            const int direction,
                            BtreeLocation* locOut) = 0;

        // TODO: expose full set of args for testing?
        virtual void fullValidate(long long* numKeysOut) = 0;

        virtual bool isEmpty() = 0;

        /**
         * Return OK if it's not
         * Otherwise return a status that can be displayed 
         */
        virtual Status dupKeyCheck(const BSONObj& key, const DiskLoc& loc) = 0;
    };

}  // namespace transition
}  // namespace mongo
