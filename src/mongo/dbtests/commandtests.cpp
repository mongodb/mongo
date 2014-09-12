/**
 *    Copyright (C) 2010 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/dbtests/dbtests.h"

using namespace mongo;

namespace CommandTests {
    // one namespace per command
    namespace FileMD5 {
        struct Base {
            Base() : db(&_txn) {
                db.dropCollection(ns());
                db.ensureIndex(ns(), BSON( "files_id" << 1 << "n" << 1 ));
            }

            const char* ns() { return "test.fs.chunks"; }

            OperationContextImpl _txn;
            DBDirectClient db;
        };
        struct Type0 : Base {
            void run() {
                {
                    BSONObjBuilder b;
                    b.genOID();
                    b.append("files_id", 0);
                    b.append("n", 0);
                    b.appendBinData("data", 6, BinDataGeneral, "hello ");
                    db.insert(ns(), b.obj());
                }
                {
                    BSONObjBuilder b;
                    b.genOID();
                    b.append("files_id", 0);
                    b.append("n", 1);
                    b.appendBinData("data", 5, BinDataGeneral, "world");
                    db.insert(ns(), b.obj());
                }

                BSONObj result;
                ASSERT( db.runCommand("test", BSON("filemd5" << 0), result) );
                ASSERT_EQUALS( string("5eb63bbbe01eeed093cb22bb8f5acdc3") , result["md5"].valuestr() );
            }
        };
        struct Type2 : Base {
            void run() {
                {
                    BSONObjBuilder b;
                    b.genOID();
                    b.append("files_id", 0);
                    b.append("n", 0);
                    b.appendBinDataArrayDeprecated("data", "hello ", 6);
                    db.insert(ns(), b.obj());
                }
                {
                    BSONObjBuilder b;
                    b.genOID();
                    b.append("files_id", 0);
                    b.append("n", 1);
                    b.appendBinDataArrayDeprecated("data", "world", 5);
                    db.insert(ns(), b.obj());
                }

                BSONObj result;
                ASSERT( db.runCommand("test", BSON("filemd5" << 0), result) );
                ASSERT_EQUALS( string("5eb63bbbe01eeed093cb22bb8f5acdc3") , result["md5"].valuestr() );
            }
        };
    }

    class All : public Suite {
    public:
        All() : Suite( "commands" ) {
        }

        void setupTests() {
            add< FileMD5::Type0 >();
            add< FileMD5::Type2 >();
        }

    } all;
}
