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
#include "../util/alignedbuilder.h"
#include "../util/mongoutils/str.h"
#include "../util/file.h"
#include "durop.h"

using namespace mongoutils;

namespace mongo { 

    extern string dbpath; // --dbpath parm

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
            default:
                massert(13546, str::stream() << "dur recover unrecognized opcode in journal " << opcode, false);
            }
            return op;
        }

        void DurOp::serialize(AlignedBuilder& ab) { 
            ab.appendNum(_opcode);
            _serialize(ab);
        }

        FileCreatedOp::FileCreatedOp(BufReader& log) : DurOp(JEntry::OpCode_FileCreated) { 
            unsigned long long reserved;
            log.read(reserved);
            log.read(reserved);
            log.read(_len); // size of file, not length of name
            log.readStr(_filename);
        }

        void FileCreatedOp::_serialize(AlignedBuilder& ab) {
            ab.appendNum((unsigned long long) 0); // reserved for future use
            ab.appendNum((unsigned long long) 0); // reserved for future use
            ab.appendNum(_len);
            /*string fn;
            if( str::startsWith(_filename, dbpath) )
                fn = str::after(_filename, dbpath);
            else {
                fn = _filename;
                log() << "warning logging creation of file " << _filename << " which is not below dbpath " << dbpath << endl;
            }*/
            ab.appendStr(_filename);
        }

        string FileCreatedOp::toString() { 
            return str::stream() << "FileCreatedOp " << _filename << ' ' << _len/1024.0/1024.0;
        }

        void FileCreatedOp::replay() { 
            // i believe the code assumes new files are filled with zeros.  thus we have to recreate the file,
            // or rewrite at least, even if it were the right length.  perhaps one day we should change that
            // although easier to avoid defects if we assume it is zeros perhaps.
            try { 
                remove(_filename);
            }
            catch(std::exception& e) { 
                log(1) << "recover info FileCreateOp::replay unlink " << e.what() << endl;
            }

            log() << "recover create file " << _filename << ' ' << _len/1024.0/1024.0 << "MB" << endl;
            File f;
            f.open(_filename.c_str());
            massert(13547, str::stream() << "recover couldn't create file " << _filename, f.is_open());
            unsigned long long left = _len;
            const unsigned blksz = 64 * 1024;
            scoped_ptr<char> v( new char[blksz] );
            memset( v.get(), 0, blksz );
            fileofs ofs = 0;
            while( left ) { 
                unsigned long long w = left < blksz ? left : blksz;
                f.write(ofs, v.get(), (unsigned) w);
                left -= w;
                ofs += w;
            }
            f.fsync();
        }

    }

}
