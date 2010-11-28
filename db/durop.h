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
#include "bufreader.h"

namespace mongo {

    class AlignedBuilder;
    class BufReader;

    namespace dur {

        const unsigned Alignment = 8192;

        /** DurOp - Operations we journal that aren't just basic writes.  
         *
         *  Basic writes are logged as JEntry's, and indicated in ram temporarily as struct dur::WriteIntent.
         *  We don't make WriteIntent inherit from DurOp to keep it as lean as possible as there will be millions of 
         *  them (we don't want a vtable for example there).
         * 
         *  For each op we want to journal, we define a subclass.
         */
        class DurOp /* copyable */ { 
        public:
            // @param opcode a sentinel value near max unsigned which uniquely idenfies the operation.
            // @see dur::JEntry
            DurOp(unsigned opcode) : _opcode(opcode) { }

            virtual ~DurOp() { }

            /** serialize the op out to a builder which will then be written (presumably) to the journal */
            void serialize(AlignedBuilder& ab);

            /** read a durop from journal file referened by br.
                @param opcode the opcode which has already been written from the bufreader
            */
            static shared_ptr<DurOp> read(unsigned opcode, BufReader& br);

            /** replay the operation (during recovery) 
                throws
            */
            virtual void replay() = 0;

            virtual string toString() = 0;

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
            FileCreatedOp(string f, unsigned long long l) : 
              DurOp(JEntry::OpCode_FileCreated), _filename(f), _len(l)  { }

            virtual void replay();

            virtual string toString();

        protected:
            virtual void _serialize(AlignedBuilder& ab);

        private:
            string _filename;
            unsigned long long _len;
        };

    }

}
