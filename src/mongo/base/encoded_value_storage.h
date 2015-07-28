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

#include <cstring>

#include "mongo/base/data_view.h"

namespace mongo {

struct ZeroInitTag_t {
    ZeroInitTag_t(){};
};

const ZeroInitTag_t kZeroInitTag;

template <typename Layout, typename ConstView, typename View>
class EncodedValueStorage {
protected:
    EncodedValueStorage() {}

    // This explicit constructor is provided to allow for easy zeroing
    // during creation of a value.  You might prefer this over an
    // uninitialised value if the zeroed version provides a useful base
    // state.  Such cases might include a set of counters that begin at
    // zero, flags that start off false or a larger structure where some
    // significant portion of storage falls into those kind of use cases.
    // Use this where you might have used calloc(1, sizeof(type)) in C.
    //
    // The added value of providing it as a constructor lies in the ability
    // of subclasses to easily inherit a zeroed base state during
    // initialization.
    explicit EncodedValueStorage(ZeroInitTag_t) {
        std::memset(_data, 0, sizeof(_data));
    }

public:
    View view() {
        return _data;
    }

    ConstView constView() const {
        return _data;
    }

    operator View() {
        return view();
    }

    operator ConstView() const {
        return constView();
    }

private:
    char _data[sizeof(Layout)];
};

}  // namespace mongo
