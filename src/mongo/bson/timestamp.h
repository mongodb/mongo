/*    Copyright 2009 10gen Inc.
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

#include "mongo/base/data_view.h"
#include "mongo/bson/util/builder.h"
#include "mongo/util/assert_util.h"

namespace mongo {

    /* Timestamp: A combination of current second plus an ordinal value.
     */
#pragma pack(4)
    class Timestamp {
        unsigned i; // ordinal comes first so we can do a single 64 bit compare on little endian
        unsigned secs;
    public:
        unsigned getSecs() const {
            return secs;
        }
        unsigned getInc() const {
            return i;
        }

        Timestamp(Date_t date) {
            reinterpret_cast<unsigned long long&>(*this) = date.millis;
            dassert( (int)secs >= 0 );
        }

        Timestamp(unsigned a, unsigned b) {
            secs = a;
            i = b;
            dassert( (int)secs >= 0 );
        }
        Timestamp( const Timestamp& other ) { 
            secs = other.secs;
            i = other.i;
            dassert( (int)secs >= 0 );
        }
        Timestamp() {
            secs = 0;
            i = 0;
        }

        // Maximum Timestamp value.
        static Timestamp max();

        unsigned long long asULL() const {
            return reinterpret_cast<const unsigned long long*>(&i)[0];
        }
        long long asLL() const {
            return reinterpret_cast<const long long*>(&i)[0];
        }

        bool isNull() const { return secs == 0; }

        std::string toStringLong() const;

        std::string toStringPretty() const;

        std::string toString() const;

        bool operator==(const Timestamp& r) const {
            return i == r.i && secs == r.secs;
        }
        bool operator!=(const Timestamp& r) const {
            return !(*this == r);
        }
        bool operator<(const Timestamp& r) const {
            if ( secs != r.secs )
                return secs < r.secs;
            return i < r.i;
        }
        bool operator<=(const Timestamp& r) const {
            return *this < r || *this == r;
        }
        bool operator>(const Timestamp& r) const {
            return !(*this <= r);
        }
        bool operator>=(const Timestamp& r) const {
            return !(*this < r);
        }

        // Append the BSON representation of this Timestamp to the given BufBuilder with the given
        // name. This lives here because Timestamp manages its own serialization format.
        void append(BufBuilder& builder, const StringData& fieldName) const;

    };
#pragma pack()

} // namespace mongo
