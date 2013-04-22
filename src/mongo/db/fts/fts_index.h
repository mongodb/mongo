// fts_index.h

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
*/

#pragma once

#include <map>
#include <vector>

#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/fts/fts_util.h"
#include "mongo/db/fts/stemmer.h"
#include "mongo/db/fts/stop_words.h"
#include "mongo/db/fts/tokenizer.h"
#include "mongo/db/index.h"

namespace mongo {

    namespace fts {

        class FTSIndex : public IndexType {
        public:

            // index constructor, called when user enters ensureIndex command with fts flag
            FTSIndex(const IndexPlugin *plugin, const IndexSpec* spec);

            void getKeys( const BSONObj& obj, BSONObjSet& keys) const;

            const FTSSpec& getFtsSpec() const { return _ftsSpec; }

        private:

            FTSSpec _ftsSpec;
        };


        class FTSIndexPlugin : public IndexPlugin {
        public:
            FTSIndexPlugin();

            IndexType* generate( const IndexSpec* spec ) const;

            BSONObj adjustIndexSpec( const BSONObj& spec ) const;

            void postBuildHook( const IndexSpec& spec ) const;

        };

    } //namespace fts
} //namespace mongo
