// @file durop.h class DurOp and descendants

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

#pragma once

#include "dur_journalformat.h"
#include "../util/bufreader.h"
#include "../util/paths.h"

namespace mongo {

    class AlignedBuilder;

    namespace dur {

        /** DurOp - Operations we journal that aren't just basic writes.
         *
         *  Basic writes are logged as JEntry's, and indicated in ram temporarily as struct dur::WriteIntent.
         *  We don't make WriteIntent inherit from DurOp to keep it as lean as possible as there will be millions of
         *  them (we don't want a vtable for example there).
         *
         *  For each op we want to journal, we define a subclass.
         */
        class DurOp { /* copyable */
        public:
            // @param opcode a sentinel value near max unsigned which uniquely identifies the operation.
            // @see dur::JEntry
            DurOp(unsigned opcode) : _opcode(opcode) { }

            virtual ~DurOp() { }

            /** serialize the op out to a builder which will then be written (presumably) to the journal */
            void serialize(AlignedBuilder& ab);

            /** read a durop from journal file referenced by br.
                @param opcode the opcode which has already been written from the bufreader
            */
            static shared_ptr<DurOp> read(unsigned opcode, BufReader& br);

            /** replay the operation (during recovery)
                throws

                For now, these are not replayed during the normal WRITETODATAFILES phase, since these
                operations are handled in other parts of the code. At some point this may change.
            */
            virtual void replay() = 0;

            virtual string toString() = 0;

            /** if the op requires all file to be closed before doing its work, returns true. */
            virtual bool needFilesClosed() { return false; }

        protected:
            /** DurOp will have already written the opcode for you */
            virtual void _serialize(AlignedBuilder& ab) = 0;

        private:
            const unsigned _opcode;
        };

        /** indicates creation of a new file */
        class FileCreatedOp : public DurOp {
        public:
            FileCreatedOp(BufReader& log);
            /** param f filename to create with path */
            FileCreatedOp(string f, unsigned long long l);
            virtual void replay();
            virtual string toString();
            virtual bool needFilesClosed();
        protected:
            virtual void _serialize(AlignedBuilder& ab);
        private:
            RelativePath _p;
            unsigned long long _len; // size of file, not length of name
        };

        /** record drop of a database */
        class DropDbOp : public DurOp {
        public:
            DropDbOp(BufReader& log);
            DropDbOp(string db) :
                DurOp(JEntry::OpCode_DropDb), _db(db) { }
            virtual void replay();
            virtual string toString() { return string("DropDbOp ") + _db; }
            virtual bool needFilesClosed() { return true; }
        protected:
            virtual void _serialize(AlignedBuilder& ab);
        private:
            string _db;
        };

    }

}
