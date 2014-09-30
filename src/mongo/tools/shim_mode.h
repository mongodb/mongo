/*    Copyright 2014 MongoDB Inc.
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

#pragma once

#include <string>

namespace mongo {

    /**
     * Mongoshim modes.
     */
    class ShimMode {
    public:
        enum Value {
            kFind = 0, // Reads documents from collection.
            kInsert = 1, // Inserts documents into collection.
            kUpsert = 2, // Updates/inserts documents in collection.
            kRemove = 3, // Removes documents from collection.
            kRepair = 4, // Reads uncorrupted documents from collection record store.
            kCommand = 5, // Runs a command on the database.
            kNumShimModes
        };

        /* implicit */ ShimMode(Value value) : _value(value) {}

        operator Value() const { return _value; }

        /**
         * Returns string representation shim mode.
         * Used to generate list of choices for command line option.
         */
        std::string toString() const;

    private:
        Value _value;
    };

}  // namespace mongo
