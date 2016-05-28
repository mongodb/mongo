/*    Copyright 2012 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/base/data_view.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/random.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"

namespace {

using namespace mongo;
using std::unique_ptr;
using std::endl;

void appendInvalidStringElement(const char* fieldName, BufBuilder* bb) {
    // like a BSONObj string, but without a NUL terminator.
    bb->appendChar(String);
    bb->appendStr(fieldName, /*withNUL*/ true);
    bb->appendNum(4);
    bb->appendStr("asdf", /*withNUL*/ false);
}

TEST(BSONValidate, Basic) {
    BSONObj x;
    ASSERT_TRUE(x.valid());

    x = BSON("x" << 1);
    ASSERT_TRUE(x.valid());
}

TEST(BSONValidate, RandomData) {
    PseudoRandom r(17);

    int numValid = 0;
    int numToRun = 1000;
    long long jsonSize = 0;

    for (int i = 0; i < numToRun; i++) {
        int size = 1234;

        char* x = new char[size];
        DataView(x).write(tagLittleEndian(size));

        for (int i = 4; i < size; i++) {
            x[i] = r.nextInt32(255);
        }

        x[size - 1] = 0;

        BSONObj o(x);

        ASSERT_EQUALS(size, o.objsize());

        if (o.valid()) {
            numValid++;
            jsonSize += o.jsonString().size();
            ASSERT_OK(validateBSON(o.objdata(), o.objsize()));
        } else {
            ASSERT_NOT_OK(validateBSON(o.objdata(), o.objsize()));
        }

        delete[] x;
    }

    log() << "RandomData: didn't crash valid/total: " << numValid << "/" << numToRun
          << " (want few valid ones)"
          << " jsonSize: " << jsonSize << endl;
}

TEST(BSONValidate, MuckingData1) {
    BSONObj theObject;

    {
        BSONObjBuilder b;
        b.append("name", "eliot was here");
        b.append("yippee", "asd");
        BSONArrayBuilder a(b.subarrayStart("arr"));
        for (int i = 0; i < 100; i++) {
            a.append(BSON("x" << i << "who"
                              << "me"
                              << "asd"
                              << "asd"));
        }
        a.done();
        b.done();

        theObject = b.obj();
    }

    int numValid = 0;
    int numToRun = 1000;
    long long jsonSize = 0;

    for (int i = 4; i < theObject.objsize() - 1; i++) {
        BSONObj mine = theObject.copy();

        char* data = const_cast<char*>(mine.objdata());

        data[i] = 200;

        numToRun++;
        if (mine.valid()) {
            numValid++;
            jsonSize += mine.jsonString().size();
            ASSERT_OK(validateBSON(mine.objdata(), mine.objsize()));
        } else {
            ASSERT_NOT_OK(validateBSON(mine.objdata(), mine.objsize()));
        }
    }

    log() << "MuckingData1: didn't crash valid/total: " << numValid << "/" << numToRun
          << " (want few valid ones) "
          << " jsonSize: " << jsonSize << endl;
}

TEST(BSONValidate, Fuzz) {
    int64_t seed = time(0);
    log() << "BSONValidate Fuzz random seed: " << seed << endl;
    PseudoRandom randomSource(seed);

    BSONObj original = BSON("one" << 3 << "two" << 5 << "three" << BSONObj() << "four"
                                  << BSON("five" << BSON("six" << 11))
                                  << "seven"
                                  << BSON_ARRAY("a"
                                                << "bb"
                                                << "ccc"
                                                << 5)
                                  << "eight"
                                  << BSONDBRef("rrr", OID("01234567890123456789aaaa"))
                                  << "_id"
                                  << OID("deadbeefdeadbeefdeadbeef")
                                  << "nine"
                                  << BSONBinData("\x69\xb7", 2, BinDataGeneral)
                                  << "ten"
                                  << Date_t::fromMillisSinceEpoch(44)
                                  << "eleven"
                                  << BSONRegEx("foooooo", "i"));

    int32_t fuzzFrequencies[] = {2, 10, 20, 100, 1000};
    for (size_t i = 0; i < sizeof(fuzzFrequencies) / sizeof(int32_t); ++i) {
        int32_t fuzzFrequency = fuzzFrequencies[i];

        // Copy the 'original' BSONObj to 'buffer'.
        unique_ptr<char[]> buffer(new char[original.objsize()]);
        memcpy(buffer.get(), original.objdata(), original.objsize());

        // Randomly flip bits in 'buffer', with probability determined by 'fuzzFrequency'. The
        // first four bytes, representing the size of the object, are excluded from bit
        // flipping.
        for (int32_t byteIdx = 4; byteIdx < original.objsize(); ++byteIdx) {
            for (int32_t bitIdx = 0; bitIdx < 8; ++bitIdx) {
                if (randomSource.nextInt32(fuzzFrequency) == 0) {
                    reinterpret_cast<unsigned char&>(buffer[byteIdx]) ^= (1U << bitIdx);
                }
            }
        }
        BSONObj fuzzed(buffer.get());

        // Check that the two validation implementations agree (and neither crashes).
        ASSERT_EQUALS(fuzzed.valid(), validateBSON(fuzzed.objdata(), fuzzed.objsize()).isOK());
    }
}

TEST(BSONValidateFast, Empty) {
    BSONObj x;
    ASSERT_OK(validateBSON(x.objdata(), x.objsize()));
}

TEST(BSONValidateFast, RegEx) {
    BSONObjBuilder b;
    b.appendRegex("foo", "i");
    BSONObj x = b.obj();
    ASSERT_OK(validateBSON(x.objdata(), x.objsize()));
}

TEST(BSONValidateFast, Simple0) {
    BSONObj x;
    ASSERT_OK(validateBSON(x.objdata(), x.objsize()));

    x = BSON("foo" << 17 << "bar"
                   << "eliot");
    ASSERT_OK(validateBSON(x.objdata(), x.objsize()));
}

TEST(BSONValidateFast, Simple2) {
    char buf[64];
    for (int i = 1; i <= JSTypeMax; i++) {
        BSONObjBuilder b;
        sprintf(buf, "foo%d", i);
        b.appendMinForType(buf, i);
        sprintf(buf, "bar%d", i);
        b.appendMaxForType(buf, i);
        BSONObj x = b.obj();
        ASSERT_OK(validateBSON(x.objdata(), x.objsize()));
    }
}


TEST(BSONValidateFast, Simple3) {
    BSONObjBuilder b;
    char buf[64];
    for (int i = 1; i <= JSTypeMax; i++) {
        sprintf(buf, "foo%d", i);
        b.appendMinForType(buf, i);
        sprintf(buf, "bar%d", i);
        b.appendMaxForType(buf, i);
    }
    BSONObj x = b.obj();
    ASSERT_OK(validateBSON(x.objdata(), x.objsize()));
}

TEST(BSONValidateFast, NestedObject) {
    BSONObj x = BSON("a" << 1 << "b" << BSON("c" << 2 << "d" << BSONArrayBuilder().obj() << "e"
                                                 << BSON_ARRAY("1" << 2 << 3)));
    ASSERT_OK(validateBSON(x.objdata(), x.objsize()));
    ASSERT_NOT_OK(validateBSON(x.objdata(), x.objsize() / 2));
}

TEST(BSONValidateFast, ErrorWithId) {
    BufBuilder bb;
    BSONObjBuilder ob(bb);
    ob.append("_id", 1);
    appendInvalidStringElement("not_id", &bb);
    const BSONObj x = ob.done();
    const Status status = validateBSON(x.objdata(), x.objsize());
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(status.reason(), "not null terminated string in object with _id: 1");
}

TEST(BSONValidateFast, ErrorBeforeId) {
    BufBuilder bb;
    BSONObjBuilder ob(bb);
    appendInvalidStringElement("not_id", &bb);
    ob.append("_id", 1);
    const BSONObj x = ob.done();
    const Status status = validateBSON(x.objdata(), x.objsize());
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(status.reason(), "not null terminated string in object with unknown _id");
}

TEST(BSONValidateFast, ErrorNoId) {
    BufBuilder bb;
    BSONObjBuilder ob(bb);
    appendInvalidStringElement("not_id", &bb);
    const BSONObj x = ob.done();
    const Status status = validateBSON(x.objdata(), x.objsize());
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(status.reason(), "not null terminated string in object with unknown _id");
}

TEST(BSONValidateFast, ErrorIsInId) {
    BufBuilder bb;
    BSONObjBuilder ob(bb);
    appendInvalidStringElement("_id", &bb);
    const BSONObj x = ob.done();
    const Status status = validateBSON(x.objdata(), x.objsize());
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(status.reason(), "not null terminated string in object with unknown _id");
}

TEST(BSONValidateFast, NonTopLevelId) {
    BufBuilder bb;
    BSONObjBuilder ob(bb);
    ob.append("not_id1",
              BSON("_id"
                   << "not the real _id"));
    appendInvalidStringElement("not_id2", &bb);
    const BSONObj x = ob.done();
    const Status status = validateBSON(x.objdata(), x.objsize());
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(status.reason(), "not null terminated string in object with unknown _id");
}

TEST(BSONValidateFast, StringHasSomething) {
    BufBuilder bb;
    BSONObjBuilder ob(bb);
    bb.appendChar(String);
    bb.appendStr("x", /*withNUL*/ true);
    bb.appendNum(0);
    const BSONObj x = ob.done();
    ASSERT_EQUALS(5  // overhead
                      +
                      1  // type
                      +
                      2  // name
                      +
                      4  // size
                  ,
                  x.objsize());
    ASSERT_NOT_OK(validateBSON(x.objdata(), x.objsize()));
}

TEST(BSONValidateBool, BoolValuesAreValidated) {
    BSONObjBuilder bob;
    bob.append("x", false);
    const BSONObj obj = bob.done();
    ASSERT_OK(validateBSON(obj.objdata(), obj.objsize()));
    const BSONElement x = obj["x"];
    // Legal, because we know that the BufBuilder gave
    // us back some heap memory, which isn't oringinally const.
    auto writable = const_cast<char*>(x.value());
    for (int val = std::numeric_limits<char>::min();
         val != (int(std::numeric_limits<char>::max()) + 1);
         ++val) {
        *writable = static_cast<char>(val);
        if ((val == 0) || (val == 1)) {
            ASSERT_OK(validateBSON(obj.objdata(), obj.objsize()));
        } else {
            ASSERT_NOT_OK(validateBSON(obj.objdata(), obj.objsize()));
        }
    }
}

}  // namespace
