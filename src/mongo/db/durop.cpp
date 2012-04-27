// @file durop.cpp

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

#include "pch.h"
#include "d_concurrency.h"
#include "../util/alignedbuilder.h"
#include "../util/mongoutils/str.h"
#include "../util/file.h"
#include "mongommf.h"
#include "durop.h"
#include "../util/file_allocator.h"

using namespace mongoutils;

#include <boost/filesystem/operations.hpp>

namespace mongo {

    extern string dbpath; // --dbpath parm

    void _deleteDataFiles(const char *);

    namespace dur {

        /** read a durop from journal file referenced by br.
            @param opcode the opcode which has already been written from the bufreader
        */
        shared_ptr<DurOp> DurOp::read(unsigned opcode, BufReader& br) {
            shared_ptr<DurOp> op;
            switch( opcode ) {
            case JEntry::OpCode_FileCreated:
                op = shared_ptr<DurOp>( new FileCreatedOp(br) );
                break;
            case JEntry::OpCode_DropDb:
                op = shared_ptr<DurOp>( new DropDbOp(br) );
                break;
            default:
                massert(13546, (str::stream() << "journal recover: unrecognized opcode in journal " << opcode), false);
            }
            return op;
        }

        void DurOp::serialize(AlignedBuilder& ab) {
            ab.appendNum(_opcode);
            _serialize(ab);
        }

        DropDbOp::DropDbOp(BufReader& log) : DurOp(JEntry::OpCode_DropDb) {
            unsigned long long reserved;
            log.read(reserved);
            log.read(reserved);
            log.readStr(_db);
            string reservedStr;
            log.readStr(reservedStr);
        }

        void DropDbOp::_serialize(AlignedBuilder& ab) {
            ab.appendNum((unsigned long long) 0); // reserved for future use
            ab.appendNum((unsigned long long) 0); // reserved for future use
            ab.appendStr(_db);
            ab.appendStr(""); // reserved
        }

        /** throws */
        void DropDbOp::replay() {
            log() << "recover replay drop db " << _db << endl;
            _deleteDataFiles(_db.c_str());
        }

        FileCreatedOp::FileCreatedOp(string f, unsigned long long l) :
            DurOp(JEntry::OpCode_FileCreated) {
            _p = RelativePath::fromFullPath(f);
            _len = l;
        }

        FileCreatedOp::FileCreatedOp(BufReader& log) : DurOp(JEntry::OpCode_FileCreated) {
            unsigned long long reserved;
            log.read(reserved);
            log.read(reserved);
            log.read(_len); // size of file, not length of name
            string s;
            log.readStr(s);
            _p._p = s;
        }

        void FileCreatedOp::_serialize(AlignedBuilder& ab) {
            ab.appendNum((unsigned long long) 0); // reserved for future use
            ab.appendNum((unsigned long long) 0); // reserved for future use
            ab.appendNum(_len);
            ab.appendStr(_p.toString());
        }

        string FileCreatedOp::toString() {
            return str::stream() << "FileCreatedOp " << _p.toString() << ' ' << _len/1024.0/1024.0 << "MB";
        }

        // if an operation deletes or creates a file (or moves etc.), it may need files closed.
        bool FileCreatedOp::needFilesClosed() {
            return boost::filesystem::exists( _p.asFullPath() );
        }

        void FileCreatedOp::replay() {
            // i believe the code assumes new files are filled with zeros.  thus we have to recreate the file,
            // or rewrite at least, even if it were the right length.  perhaps one day we should change that
            // although easier to avoid defects if we assume it is zeros perhaps.
            string full = _p.asFullPath();
            if( boost::filesystem::exists(full) ) {
                try {
                    boost::filesystem::remove(full);
                }
                catch(std::exception& e) {
                    log(1) << "recover info FileCreateOp::replay unlink " << e.what() << endl;
                }
            }

            log() << "recover create file " << full << ' ' << _len/1024.0/1024.0 << "MB" << endl;
            if( boost::filesystem::exists(full) ) {
                // first delete if exists.
                try {
                    boost::filesystem::remove(full);
                }
                catch(...) {
                    log() << "warning could not delete file " << full << endl;
                }
            }
            ensureParentDirCreated(full);
            File f;
            f.open(full.c_str());
            massert(13547, str::stream() << "recover couldn't create file " << full, f.is_open());
            unsigned long long left = _len;
            const unsigned blksz = 64 * 1024;
            scoped_array<char> v( new char[blksz] );
            memset( v.get(), 0, blksz );
            fileofs ofs = 0;
            while( left ) {
                unsigned long long w = left < blksz ? left : blksz;
                f.write(ofs, v.get(), (unsigned) w);
                left -= w;
                ofs += w;
            }
            f.fsync();
            flushMyDirectory(full);
            massert(13628, str::stream() << "recover failure writing file " << full, !f.bad() );
        }

    }

}
