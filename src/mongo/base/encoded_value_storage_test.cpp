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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/base/encoded_value_storage.h"

#include <cstring>

#include "mongo/base/data_type_endian.h"
#include "mongo/platform/endian.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

// Simplistic encoded value view and value classes to test EncodedValueStorage
namespace EncodedValueStorageTest {

#pragma pack(1)
struct Layout {
    uint32_t native;
    uint32_t le;
    uint32_t be;
};
#pragma pack()

class ConstView {
public:
    typedef ConstDataView view_type;

    ConstView(const char* data) : _data(data) {}

    const char* view2ptr() const {
        return data().view();
    }

    uint32_t getNative() {
        return data().read<uint32_t>(offsetof(Layout, native));
    }

    uint32_t getLE() {
        return data().read<LittleEndian<uint32_t>>(offsetof(Layout, le));
    }

    uint32_t getBE() {
        return data().read<BigEndian<uint32_t>>(offsetof(Layout, be));
    }

protected:
    const view_type& data() const {
        return _data;
    }

private:
    view_type _data;
};

class View : public ConstView {
public:
    typedef DataView view_type;

    View(char* data) : ConstView(data) {}

    using ConstView::view2ptr;
    char* view2ptr() {
        return data().view();
    }

    void setNative(uint32_t value) {
        data().write(value, offsetof(Layout, native));
    }

    void setLE(uint32_t value) {
        data().write(tagLittleEndian(value), offsetof(Layout, le));
    }

    void setBE(uint32_t value) {
        data().write(tagBigEndian(value), offsetof(Layout, be));
    }

private:
    view_type data() const {
        return const_cast<char*>(ConstView::view2ptr());
    }
};

class Value : public EncodedValueStorage<Layout, ConstView, View> {
public:
    Value() {
        static_assert(sizeof(Value) == sizeof(Layout), "sizeof(Value) == sizeof(Layout)");
    }

    Value(ZeroInitTag_t zit) : EncodedValueStorage<Layout, ConstView, View>(zit) {}
};
}

TEST(EncodedValueStorage, EncodedValueStorage) {
    EncodedValueStorageTest::Value raw;
    EncodedValueStorageTest::Value zerod(kZeroInitTag);
    char buf[sizeof(EncodedValueStorageTest::Layout)] = {0};

    ASSERT_EQUALS(raw.view().view2ptr(), raw.constView().view2ptr());

    // ensure zeroing with the init tag works
    ASSERT_EQUALS(std::memcmp(zerod.view().view2ptr(), buf, sizeof(buf)), 0);

    // see if value assignment and view() works
    zerod.view().setNative(1234);
    EncodedValueStorageTest::View(buf).setNative(1234);
    raw = zerod;
    ASSERT_EQUALS(std::memcmp(raw.view().view2ptr(), buf, sizeof(buf)), 0);

    // see if view() and constView() work appropriately
    raw.view().setNative(1);
    raw.view().setLE(2);
    raw.view().setBE(3);
    ASSERT_EQUALS(static_cast<uint32_t>(1), raw.constView().getNative());
    ASSERT_EQUALS(static_cast<uint32_t>(2), raw.constView().getLE());
    ASSERT_EQUALS(static_cast<uint32_t>(3), raw.constView().getBE());
}

}  // namespace mongo
