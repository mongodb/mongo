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

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/bson_field.h"

namespace mongo {

    /**
     * A WriteBatch is a list of writes (of a single type, targeting a single collection), with
     * associated options.  WriteBatch objects are data structures created from raw BSON in
     * the write command format, and are used as a container for WriteItem objects.
     *
     * Example usage:
     *
     *     WriteBatch wb("db.collection", WriteBatch::WRITE_INSERT);
     *     string errMsg;
     *     bool ret = wb.parse(fromjson("{insert: \"collection\",
     *                                    documents: [{_id: 0},{_id: 1}}],
     *                                    writeConcern: {w: \"majority\"},
     *                                    continueOnError: false}"),
     *                         &errMsg);
     *     if (!ret) {
     *         // errMsg ...
     *     }
     *     else {
     *         for (size_t i = 0; i < wb.getNumWriteItems(); i++) {
     *             // wb.getWriteItem(i) ...
     *         }
     *     }
     *
     */
    class WriteBatch {
        MONGO_DISALLOW_COPYING(WriteBatch);
    public:
        enum WriteType {WRITE_INSERT, WRITE_UPDATE, WRITE_DELETE};

        /**
         * Constructs a WriteBatch for operations of type "writeType" for the fully-qualified
         * namespace "ns".  Expects "ns" to be well-formed.
         *
         * Need to complete initialization by calling parse() on the newly-constructed object.
         */
        WriteBatch(const StringData& ns, WriteType writeType);

        ~WriteBatch();

        /**
         * Completes initialization by parsing associated command object.
         *
         * Returns false (and sets errMsg appropriately) on parsing error, else returns true.
         * Can only be called once per object.
         */
        bool parse(const BSONObj& cmdObj, string* errMsg);

        /**
         * Returns a single write item in the contained batch.
         *
         * n must be less than numWriteItems().
         */
        class WriteItem;
        const WriteItem& getWriteItem(size_t n) const;

        //
        // Accessors
        //

        WriteType getWriteType() const;

        const string& getNS() const;

        bool getContinueOnError() const;

        const BSONObj& getWriteConcern() const;

        size_t getNumWriteItems() const;

    private:
        //
        // Expected fields for the write cammand format.
        //

        static const BSONField<std::vector<BSONObj> > insertContainerField;
        static const BSONField<std::vector<BSONObj> > updateContainerField;
        static const BSONField<std::vector<BSONObj> > deleteContainerField;

        static const BSONField<BSONObj> writeConcernField;

        static const BSONField<bool> continueOnErrorField;

        // Namespace to target (fully-qualified, e.g. "test.foo").
        const string _ns;

        // Type of write (e.g. insert).
        //
        // All write items contained will also have this write type.
        const WriteType _writeType;

        // When a write item fails execution, whether or not to process subsequent items.
        bool _continueOnError;

        // Write concern to satisfy, e.g. (e.g. {w:2, wtimeout:1000, j:true})
        BSONObj _writeConcern;

        // List of write items.  Owned here.
        std::vector<const WriteItem*> _writeItems;
    };

    /**
     * A single item in a write batch.
     *
     * After constructing a WriteItem(), caller must verify isValid() to ensure request is
     * properly formatted.
     */
    class WriteBatch::WriteItem {
    public:
        WriteItem(WriteType writeType, const BSONObj& writeItem);

        /**
         * Returns true iff "_writeItem" is a properly-formatted object of "_writeType".
         * If not valid, fills in errMsg (if requested).
         */
        bool isValid(string* errMsg = NULL) const;

        /**
         * Accessor for _writeType.
         */
        WriteType getWriteType() const;

        /**
         * Returns true if object is valid, and fills in respective fields.
         * Returns false if object is not valid, and fills in errMsg.
         */
        bool parseInsertItem(string* errMsg, BSONObj* doc) const;

        /**
         * Returns true if object is valid, and fills in respective fields.
         * Returns false if object is not valid, and fills in errMsg.
         */
        bool parseUpdateItem(string* errMsg,
                             BSONObj* queryObj,
                             BSONObj* updateObj,
                             bool* multi,
                             bool* upsert) const;

        /**
         * Returns true if object is valid, and fills in respective fields.
         * Returns false if object is not valid, and fills in errMsg.
         */
        // TODO Add "limit" option to delete.
        bool parseDeleteItem(string* errMsg, BSONObj* queryObj) const;

    private:
        //
        // Expected fields for the write item format.
        //

        static const BSONField<BSONObj> updateQField;
        static const BSONField<BSONObj> updateUField;
        static const BSONField<bool> updateMultiField;
        static const BSONField<bool> updateUpsertField;
        static const BSONField<BSONObj> deleteQField;

        // Type of write (e.g. insert).
        WriteType _writeType;

        // Unparsed contents of write (e.g. for update, {q:{}, u:{$set:{a:1}}}).
        BSONObj _writeItem;
    };

} // namespace mongo
