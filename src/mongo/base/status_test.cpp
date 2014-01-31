/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/base/status.h"
#include "mongo/unittest/unittest.h"

namespace {

    using mongo::ErrorCodes;
    using mongo::Status;

    TEST(Basic, Accessors) {
        Status status(ErrorCodes::MaxError, "error", 9999);
        ASSERT_EQUALS(status.code(), ErrorCodes::MaxError);
        ASSERT_EQUALS(status.reason(), "error");
        ASSERT_EQUALS(status.location(), 9999);
    }

    TEST(Basic, OKIsAValidStatus) {
        Status status = Status::OK();
        ASSERT_EQUALS(status.code(), ErrorCodes::OK);
    }

    TEST(Basic, Compare) {
        Status errMax(ErrorCodes::MaxError, "error");
        ASSERT_TRUE(errMax.compare(errMax));
        ASSERT_FALSE(errMax.compare(Status::OK()));

        Status errMaxWithLoc(ErrorCodes::MaxError, "error", 9998);
        ASSERT_FALSE(errMaxWithLoc.compare(errMax));
    }

    TEST(Cloning, Copy) {
        Status orig(ErrorCodes::MaxError, "error");
        ASSERT_EQUALS(orig.refCount(), 1U);

        Status dest(orig);
        ASSERT_EQUALS(dest.code(), ErrorCodes::MaxError);
        ASSERT_EQUALS(dest.reason(), "error");

        ASSERT_EQUALS(dest.refCount(), 2U);
        ASSERT_EQUALS(orig.refCount(), 2U);
    }

    TEST(Cloning, OKIsNotRefCounted) {
        ASSERT_EQUALS(Status::OK().refCount(), 0U);

        Status myOk = Status::OK();
        ASSERT_EQUALS(myOk.refCount(), 0U);
        ASSERT_EQUALS(Status::OK().refCount(), 0U);
    }

    TEST(Parsing, CodeToEnum) {
        ASSERT_EQUALS(ErrorCodes::TypeMismatch, ErrorCodes::fromInt(ErrorCodes::TypeMismatch));
        ASSERT_EQUALS(ErrorCodes::UnknownError, ErrorCodes::fromInt(ErrorCodes::UnknownError));
        ASSERT_EQUALS(ErrorCodes::UnknownError, ErrorCodes::fromInt(ErrorCodes::MaxError));
        ASSERT_EQUALS(ErrorCodes::OK, ErrorCodes::fromInt(0));
    }

    TEST(Parsing, StringToEnum) {
        ASSERT_EQUALS(ErrorCodes::TypeMismatch, ErrorCodes::fromString("TypeMismatch"));
        ASSERT_EQUALS(ErrorCodes::UnknownError, ErrorCodes::fromString("UnknownError"));
        ASSERT_EQUALS(ErrorCodes::UnknownError, ErrorCodes::fromString("Garbage"));
        ASSERT_EQUALS(ErrorCodes::OK, ErrorCodes::fromString("OK"));
    }

} // unnamed namespace
