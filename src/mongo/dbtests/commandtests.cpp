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
 */

#include "mongo/pch.h"

#include "mongo/db/d_concurrency.h"
#include "mongo/dbtests/dbtests.h"

using namespace mongo;

namespace CommandTests {
    // one namespace per command
    namespace FileMD5 {
        struct Base {
            Base() {
                db.dropCollection(ns());
                db.ensureIndex(ns(), BSON( "files_id" << 1 << "n" << 1 ));
            }

            const char* ns() { return "test.fs.chunks"; }

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
