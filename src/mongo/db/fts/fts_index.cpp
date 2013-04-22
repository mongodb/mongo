// fts_index.cpp

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

#include "mongo/pch.h"

#include "mongo/base/init.h"
#include "mongo/db/client.h"
#include "mongo/db/fts/fts_enabled.h"
#include "mongo/db/fts/fts_index.h"
#include "mongo/db/fts/fts_index_format.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stringutils.h"
#include "mongo/util/timer.h"
#include "mongo/db/pdfile.h"

namespace mongo {

    namespace fts {

        using namespace mongoutils;

        /*
         * extrapolates the weights vector
         * and extra information from the spec
         * @param plugin the index plugin for FTS
         * @param spec the index specification
         */
        FTSIndex::FTSIndex( const IndexPlugin* plugin, const IndexSpec* spec )
            : IndexType( plugin, spec ), _ftsSpec( spec->info ) {
        }

        void FTSIndex::getKeys( const BSONObj& obj, BSONObjSet& keys) const {
            FTSIndexFormat::getKeys( _ftsSpec, obj, &keys );
        }


        FTSIndexPlugin::FTSIndexPlugin() : IndexPlugin( INDEX_NAME ) {}


        /*
         * Adjusts spec by appending information relative to the
         * FTS Index (such as weights, index name, etc)
         * @param spec, specification object
         *
         */
        BSONObj FTSIndexPlugin::adjustIndexSpec( const BSONObj& spec ) const {
            StringData desc  = cc().desc();
            if ( desc.find( "conn" ) == 0 ) {
                // this is to make sure we only complain for users
                // if you do get a text index created an a primary
                // want it to index on the secondary as well
                massert( 16633, "text search not enabled", isTextSearchEnabled() );
            }
            return FTSSpec::fixSpec( spec );
        }

        /*
         * Generates an FTSIndex with a spec and this plugin
         * @param spec, specification to be used
         */
        IndexType* FTSIndexPlugin::generate( const IndexSpec* spec ) const {
            return new FTSIndex( this, spec );
        }

        void FTSIndexPlugin::postBuildHook( const IndexSpec& spec ) const {
            string ns = spec.getDetails()->parentNS();
            NamespaceDetails* nsd = nsdetails( ns );
            if ( nsd->setUserFlag( NamespaceDetails::Flag_UsePowerOf2Sizes ) ) {
                nsd->syncUserFlags( ns );
            }
        }

        FTSIndexPlugin* ftsPlugin;
        MONGO_INITIALIZER(FTSIndexPlugin)(InitializerContext* context) {
            ftsPlugin = new FTSIndexPlugin();
            return Status::OK();
        }

    }

}
