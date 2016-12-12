/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include <memory>

#include "mongo/db/client.h"
#include "mongo/db/json.h"
#include "mongo/db/repl/noop_writer.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_executor_test_fixture.h"
#include "mongo/unittest/unittest.h"


namespace {

using namespace mongo;
using namespace mongo::repl;

using unittest::log;

class NoopWriterTest : public ReplicationExecutorTest {
public:
    NoopWriter* getNoopWriter() {
        return _noopWriter.get();
    }

protected:
    void setUp() override {
        setupNoopWriter(Seconds(1));
    }

    void tearDown() override {}

private:
    void startNoopWriter(OpTime opTime) {
        invariant(_noopWriter);
        _noopWriter->start(opTime);
    }

    void stopNoopWriter() {
        invariant(_noopWriter);
        _noopWriter->stop();
    }

    void setupNoopWriter(Seconds waitTime) {
        invariant(!_noopWriter);
        _noopWriter = stdx::make_unique<NoopWriter>(waitTime);
    }

    OpTime _getLastOpTime() {
        return _lastOpTime;
    }

    std::unique_ptr<NoopWriter> _noopWriter;
    OpTime _lastOpTime;
};
// TODO: SERVER-25679
TEST_F(NoopWriterTest, CreateDestroy) {
    NoopWriter* writer = getNoopWriter();
    ASSERT(writer != nullptr);
}
}
