// record_data.h

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

#pragma once

namespace mongo {

    /**
     * A replacement for the Record class. This class represents data in a record store.
     * The _dataPtr attribute is used to manage memory ownership. If _dataPtr is NULL, then
     * the memory pointed to by _data is owned by the RecordStore. If _dataPtr is not NULL, then
     * it must point to the same array as _data.
     */
    class RecordData {
    public:
        RecordData(const char* data, int size): _data(data), _size(size), _dataPtr() { }

        RecordData(const char* data, int size, const boost::shared_array<char>& dataPtr)
            : _data(data), _size(size), _dataPtr(dataPtr) { }

        const char* data() const { return _data; }

        int size() const { return _size; }

        /**
         * Returns true if this owns its own memory, and false otherwise
         */
        bool isOwned() const { return _dataPtr; }

        // TODO eliminate double-copying
        BSONObj toBson() const { return isOwned() ? BSONObj(_data).getOwned() : BSONObj(_data); }

    private:
        const char* _data;
        int _size;
        const boost::shared_array<char> _dataPtr;
    };

} // namespace mongo
