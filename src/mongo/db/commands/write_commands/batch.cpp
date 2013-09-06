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

#include "mongo/db/commands/write_commands/batch.h"

#include "mongo/db/field_parser.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

    const BSONField<bool> WriteBatch::continueOnErrorField("continueOnError", true);
    const BSONField<BSONObj> WriteBatch::writeConcernField("writeConcern", BSONObj());

    const BSONField<std::vector<BSONObj> > WriteBatch::insertContainerField("documents");
    const BSONField<std::vector<BSONObj> > WriteBatch::updateContainerField("updates");
    const BSONField<std::vector<BSONObj> > WriteBatch::deleteContainerField("deletes");

    const BSONField<BSONObj> WriteBatch::WriteItem::updateQField("q");
    const BSONField<BSONObj> WriteBatch::WriteItem::updateUField("u");
    const BSONField<bool> WriteBatch::WriteItem::updateMultiField("multi", false);
    const BSONField<bool> WriteBatch::WriteItem::updateUpsertField("upsert", false);

    const BSONField<BSONObj> WriteBatch::WriteItem::deleteQField("q");

    WriteBatch::WriteBatch(const StringData& ns, WriteType writeType)
        : _ns(ns.toString())
        , _writeType(writeType) {
        verify(NamespaceString(_ns).isValid());
    }

    WriteBatch::~WriteBatch() {
        for (vector<const WriteBatch::WriteItem*>::const_iterator it = _writeItems.begin();
             it != _writeItems.end();
             ++it) {
            delete *it;
        }
    }

    bool WriteBatch::parse(const BSONObj& cmdObj, string* errMsg) {
        if (FieldParser::extract(cmdObj, continueOnErrorField, &_continueOnError, errMsg) ==
            FieldParser::FIELD_INVALID) {
            return false;
        }

        if (FieldParser::extract(cmdObj, writeConcernField, &_writeConcern, errMsg) ==
            FieldParser::FIELD_INVALID) {
            return false;
        }

        // TODO Validate the following for writeConcern? Or for "legacy" support do we not want to?
        //
        // if j present, then type int or bool
        // if j present, then fsync not present
        // if fsync present, then type int or bool
        // if w present, then type string or number
        // if w is number, then positive
        // if w is string, then "majority" or configured error mode
        // if wtimeout present, then type int
        // if wtimeout present, then w present

        const BSONField<std::vector<BSONObj> > *batchField = NULL;

        switch (_writeType) {
        case WRITE_INSERT:
            batchField = &insertContainerField;
            break;
        case WRITE_UPDATE:
            batchField = &updateContainerField;
            break;
        case WRITE_DELETE:
            batchField = &deleteContainerField;
            break;
        }

        std::vector<BSONObj> writeItemsBSONObj;
        FieldParser::FieldState fieldState = FieldParser::extract(cmdObj,
                                                                  *batchField,
                                                                  &writeItemsBSONObj,
                                                                  errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) {
            return false;
        }
        else if (fieldState == FieldParser::FIELD_NONE) {
            *errMsg = mongoutils::str::stream() << "missing required field \"" <<
                      batchField->name() << "\"";
            return false;
        }

        for (std::vector<BSONObj>::const_iterator it = writeItemsBSONObj.begin();
             it != writeItemsBSONObj.end();
             ++it) {
            const WriteItem* writeItem = new WriteItem(_writeType, *it);
            _writeItems.push_back(writeItem);
            string errMsgItem;
            if (!writeItem->isValid(&errMsgItem)) {
                std::vector<BSONObj>::const_iterator begin = writeItemsBSONObj.begin();
                *errMsg = mongoutils::str::stream() << "error parsing \"" << batchField->name() <<
                          "\" element " << std::distance(begin, it) << ": " << errMsgItem;
                return false;
            }
        }

        return true;
    }

    WriteBatch::WriteType WriteBatch::getWriteType() const {
        return _writeType;
    }

    const string& WriteBatch::getNS() const {
        return _ns;
    }

    bool WriteBatch::getContinueOnError() const {
        return _continueOnError;
    }

    const BSONObj& WriteBatch::getWriteConcern() const {
        return _writeConcern;
    }

    size_t WriteBatch::getNumWriteItems() const {
        return _writeItems.size();
    }

    const WriteBatch::WriteItem& WriteBatch::getWriteItem(size_t n) const {
        verify(n < getNumWriteItems());
        return *_writeItems[n];
    }

    WriteBatch::WriteItem::WriteItem(WriteBatch::WriteType writeType, const BSONObj& writeItem)
        : _writeType(writeType), _writeItem(writeItem) {}

    bool WriteBatch::WriteItem::parseInsertItem(string* errMsg, BSONObj* doc) const {
        verify(_writeType == WRITE_INSERT);

        // Parsing an insert is trivial: copy the write item to the caller.
        *doc = _writeItem;

        return true;
    }

    bool WriteBatch::WriteItem::parseUpdateItem(string* errMsg,
                                                BSONObj* queryObj,
                                                BSONObj* updateObj,
                                                bool* multi,
                                                bool* upsert) const {
        verify(_writeType == WRITE_UPDATE);

        FieldParser::FieldState fieldState = FieldParser::extract(_writeItem,
                                                                  updateQField,
                                                                  queryObj,
                                                                  errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) {
            return false;
        }
        else if (fieldState == FieldParser::FIELD_NONE) {
            *errMsg = mongoutils::str::stream() << "missing required field \"" <<
                      updateQField.name() << "\"";
            return false;
        }

        fieldState = FieldParser::extract(_writeItem, updateUField, updateObj, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) {
            return false;
        }
        else if (fieldState == FieldParser::FIELD_NONE) {
            *errMsg = mongoutils::str::stream() << "missing required field \"" <<
                      updateUField.name() << "\"";
            return false;
        }

        if (FieldParser::extract(_writeItem, updateMultiField, multi, errMsg) ==
            FieldParser::FIELD_INVALID) {
            return false;
        }

        if (FieldParser::extract(_writeItem, updateUpsertField, upsert, errMsg) ==
            FieldParser::FIELD_INVALID) {
            return false;
        }

        return true;
    }

    bool WriteBatch::WriteItem::parseDeleteItem(string* errMsg,
                                                BSONObj* queryObj) const {
        verify(_writeType == WRITE_DELETE);

        FieldParser::FieldState fieldState = FieldParser::extract(_writeItem,
                                                                  deleteQField,
                                                                  queryObj,
                                                                  errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) {
            return false;
        }
        else if (fieldState == FieldParser::FIELD_NONE) {
            *errMsg = mongoutils::str::stream() << "missing required field \"" <<
                      deleteQField.name() << "\"";
            return false;
        }

        return true;
    }

    bool WriteBatch::WriteItem::isValid(string* errMsg) const {
        string errMsgDummy;

        if (!errMsg) {
            errMsg = &errMsgDummy;
        }

        BSONObj docDummy1;
        BSONObj docDummy2;
        bool boolDummy1;
        bool boolDummy2;

        switch (_writeType) {
        case WRITE_INSERT:
            return parseInsertItem(errMsg, &docDummy1);
        case WRITE_UPDATE:
            return parseUpdateItem(errMsg, &docDummy1, &docDummy2, &boolDummy1, &boolDummy2);
        case WRITE_DELETE:
            return parseDeleteItem(errMsg, &docDummy1);
        }

        dassert(false);
        return false;
    }

    WriteBatch::WriteType WriteBatch::WriteItem::getWriteType() const {
        return _writeType;
    }

} // namespace mongo
