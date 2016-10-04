/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include <deque>
#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/s/write_ops/batch_downconvert.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using std::vector;
using std::deque;

//
// Tests for parsing GLE responses into write errors and write concern errors for write
// commands.  These tests essentially document our expected 2.4 GLE behaviors.
//

TEST(GLEParsing, Empty) {
    const BSONObj gleResponse = fromjson("{ok: 1.0, err: null}");

    GLEErrors errors;
    ASSERT_OK(extractGLEErrors(gleResponse, &errors));
    ASSERT(!errors.writeError.get());
    ASSERT(!errors.wcError.get());
}

TEST(GLEParsing, WriteErr) {
    const BSONObj gleResponse = fromjson("{ok: 1.0, err: 'message', code: 1000}");

    GLEErrors errors;
    ASSERT_OK(extractGLEErrors(gleResponse, &errors));
    ASSERT(errors.writeError.get());
    ASSERT_EQUALS(errors.writeError->getErrMessage(), "message");
    ASSERT_EQUALS(errors.writeError->getErrCode(), 1000);
    ASSERT(!errors.wcError.get());
}

TEST(GLEParsing, JournalFail) {
    const BSONObj gleResponse = fromjson("{ok: 1.0, err: null, jnote: 'message'}");

    GLEErrors errors;
    ASSERT_OK(extractGLEErrors(gleResponse, &errors));
    ASSERT(!errors.writeError.get());
    ASSERT(errors.wcError.get());
    ASSERT_EQUALS(errors.wcError->getErrMessage(), "message");
    ASSERT_EQUALS(errors.wcError->getErrCode(), ErrorCodes::WriteConcernFailed);
}

TEST(GLEParsing, ReplErr) {
    const BSONObj gleResponse = fromjson("{ok: 1.0, err: 'norepl', wnote: 'message'}");

    GLEErrors errors;
    ASSERT_OK(extractGLEErrors(gleResponse, &errors));
    ASSERT(!errors.writeError.get());
    ASSERT(errors.wcError.get());
    ASSERT_EQUALS(errors.wcError->getErrMessage(), "message");
    ASSERT_EQUALS(errors.wcError->getErrCode(), ErrorCodes::WriteConcernFailed);
}

TEST(GLEParsing, ReplTimeoutErr) {
    const BSONObj gleResponse =
        fromjson("{ok: 1.0, err: 'timeout', errmsg: 'message', wtimeout: true}");

    GLEErrors errors;
    ASSERT_OK(extractGLEErrors(gleResponse, &errors));
    ASSERT(!errors.writeError.get());
    ASSERT(errors.wcError.get());
    ASSERT_EQUALS(errors.wcError->getErrMessage(), "message");
    ASSERT(errors.wcError->getErrInfo()["wtimeout"].trueValue());
    ASSERT_EQUALS(errors.wcError->getErrCode(), ErrorCodes::WriteConcernFailed);
}

TEST(GLEParsing, GLEFail) {
    const BSONObj gleResponse = fromjson("{ok: 0.0, err: null, errmsg: 'message', code: 1000}");

    GLEErrors errors;
    Status status = extractGLEErrors(gleResponse, &errors);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(status.reason(), "message");
    ASSERT_EQUALS(status.code(), 1000);
}

TEST(GLEParsing, GLEFailNoCode) {
    const BSONObj gleResponse = fromjson("{ok: 0.0, err: null, errmsg: 'message'}");

    GLEErrors errors;
    Status status = extractGLEErrors(gleResponse, &errors);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(status.reason(), "message");
    ASSERT_EQUALS(status.code(), ErrorCodes::UnknownError);
}

TEST(GLEParsing, NotMasterGLEFail) {
    // Not master code in response
    const BSONObj gleResponse = fromjson("{ok: 0.0, err: null, errmsg: 'message', code: 10990}");

    GLEErrors errors;
    ASSERT_OK(extractGLEErrors(gleResponse, &errors));
    ASSERT(!errors.writeError.get());
    ASSERT(errors.wcError.get());
    ASSERT_EQUALS(errors.wcError->getErrMessage(), "message");
    ASSERT_EQUALS(errors.wcError->getErrCode(), 10990);
}

TEST(GLEParsing, WriteErrWithStats) {
    const BSONObj gleResponse = fromjson("{ok: 1.0, n: 2, err: 'message', code: 1000}");

    GLEErrors errors;
    ASSERT_OK(extractGLEErrors(gleResponse, &errors));
    ASSERT(errors.writeError.get());
    ASSERT_EQUALS(errors.writeError->getErrMessage(), "message");
    ASSERT_EQUALS(errors.writeError->getErrCode(), 1000);
    ASSERT(!errors.wcError.get());
}

TEST(GLEParsing, ReplTimeoutErrWithStats) {
    const BSONObj gleResponse = fromjson(
        "{ok: 1.0, err: 'timeout', errmsg: 'message', wtimeout: true,"
        " n: 1, upserted: 'abcde'}");

    GLEErrors errors;
    ASSERT_OK(extractGLEErrors(gleResponse, &errors));
    ASSERT(!errors.writeError.get());
    ASSERT(errors.wcError.get());
    ASSERT_EQUALS(errors.wcError->getErrMessage(), "message");
    ASSERT(errors.wcError->getErrInfo()["wtimeout"].trueValue());
    ASSERT_EQUALS(errors.wcError->getErrCode(), ErrorCodes::WriteConcernFailed);
}

//
// Tests of processing and suppressing non-WC related fields from legacy GLE responses
//

TEST(LegacyGLESuppress, Basic) {
    const BSONObj gleResponse = fromjson("{ok: 1.0, err: null}");

    BSONObj stripped = stripNonWCInfo(gleResponse);
    ASSERT_EQUALS(stripped.nFields(), 2);  // with err, ok : true
    ASSERT(stripped["ok"].trueValue());
}

TEST(LegacyGLESuppress, BasicStats) {
    const BSONObj gleResponse = fromjson(
        "{ok: 0.0, err: 'message',"
        " n: 1, nModified: 1, upserted: 'abc', updatedExisting: true}");

    BSONObj stripped = stripNonWCInfo(gleResponse);
    ASSERT_EQUALS(stripped.nFields(), 1);
    ASSERT(!stripped["ok"].trueValue());
}

TEST(LegacyGLESuppress, ReplError) {
    const BSONObj gleResponse = fromjson("{ok: 0.0, err: 'norepl', n: 1, wcField: true}");

    BSONObj stripped = stripNonWCInfo(gleResponse);
    ASSERT_EQUALS(stripped.nFields(), 3);
    ASSERT(!stripped["ok"].trueValue());
    ASSERT_EQUALS(stripped["err"].str(), "norepl");
    ASSERT(stripped["wcField"].trueValue());
}

TEST(LegacyGLESuppress, StripCode) {
    const BSONObj gleResponse = fromjson("{ok: 1.0, err: 'message', code: 12345}");

    BSONObj stripped = stripNonWCInfo(gleResponse);
    ASSERT_EQUALS(stripped.nFields(), 2);  // with err, ok : true
    ASSERT(stripped["ok"].trueValue());
}

TEST(LegacyGLESuppress, TimeoutDupError24) {
    const BSONObj gleResponse = BSON("ok" << 0.0 << "err"
                                          << "message"
                                          << "code"
                                          << 12345
                                          << "err"
                                          << "timeout"
                                          << "code"
                                          << 56789
                                          << "wtimeout"
                                          << true);

    BSONObj stripped = stripNonWCInfo(gleResponse);
    ASSERT_EQUALS(stripped.nFields(), 4);
    ASSERT(!stripped["ok"].trueValue());
    ASSERT_EQUALS(stripped["err"].str(), "timeout");
    ASSERT_EQUALS(stripped["code"].numberInt(), 56789);
    ASSERT(stripped["wtimeout"].trueValue());
}
}
