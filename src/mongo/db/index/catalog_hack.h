/**
*    Copyright (C) 2013 10gen Inc.
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

#pragma once

#include "mongo/db/index/2d_access_method.h"
#include "mongo/db/index/btree_access_method.h"
#include "mongo/db/index/btree_access_method_internal.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/index/hash_access_method.h"
#include "mongo/db/index/haystack_access_method.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index/s2_access_method.h"
#include "mongo/db/index_names.h"
#include "mongo/db/pdfile.h"

namespace mongo {

    /**
     * Fake some catalog behavior until the catalog is finalized.
     */
    class CatalogHack {
    public:

        /**
         * Older versions of MongoDB treated unknown index plugins as ascending Btree indices.
         * We need to replicate this behavior.  We use the version of the on-disk database to hint
         * to us whether or not a given index was created as an actual instance of a special index,
         * or if it was just treated as an increasing Btree index.
         */
        static bool shouldOverridePlugin(const BSONObj& keyPattern) {
            string pluginName = IndexNames::findPluginName(keyPattern);
            bool known = IndexNames::isKnownName(pluginName);

            if (NULL == cc().database()) {
                return false;
            }

            const DataFileHeader* dfh = cc().database()->getFile(0)->getHeader();

            if (dfh->versionMinor == PDFILE_VERSION_MINOR_24_AND_NEWER) {
                // RulesFor24
                // This assert will be triggered when downgrading from a future version that
                // supports an index plugin unsupported by this version.
                uassert(16736, str::stream() << "Invalid index type '" << pluginName << "' "
                                             << "in index " << keyPattern,
                               known);
                return false;
            } else {
                // RulesFor22
                if (!known) {
                    log() << "warning: can't find plugin [" << pluginName << "]" << endl;
                    return true;
                }

                if (!IndexNames::existedBefore24(pluginName)) {
                    warning() << "Treating index " << keyPattern << " as ascending since "
                              << "it was created before 2.4 and '" << pluginName << "' "
                              << "was not a valid type at that time."
                              << endl;
                    return true;
                } else {
                    return false;
                }
            }
        }

        /**
         * This differs from IndexNames::findPluginName in that returns the plugin name we *should*
         * use, not the plugin name inside of the provided key pattern.  To understand when these
         * differ, see shouldOverridePlugin.
         */
        static string getAccessMethodName(const BSONObj& keyPattern) {
            if (shouldOverridePlugin(keyPattern)) {
                return "";
            } else {
                return IndexNames::findPluginName(keyPattern);
            }
        }

        static IndexDescriptor* getDescriptor(NamespaceDetails* nsd, int idxNo) {
            IndexDetails& id = nsd->idx(idxNo);
            return new IndexDescriptor(nsd, idxNo, &id, id.info.obj());
        }

        static BtreeBasedAccessMethod* getBtreeBasedIndex(IndexDescriptor* desc) {
            string type = getAccessMethodName(desc->keyPattern());

            if (IndexNames::HASHED == type) {
                return new HashAccessMethod(desc);
            } else if (IndexNames::GEO_2DSPHERE == type) {
                return new S2AccessMethod(desc);
            } else if (IndexNames::TEXT == type || IndexNames::TEXT_INTERNAL == type) {
                return new FTSAccessMethod(desc);
            } else if (IndexNames::GEO_HAYSTACK == type) {
                return new HaystackAccessMethod(desc);
            } else if ("" == type) {
                return new BtreeAccessMethod(desc);
            } else if (IndexNames::GEO_2D == type) {
                return new TwoDAccessMethod(desc);
            } else {
                cout << "Can't find index for keypattern " << desc->keyPattern() << endl;
                verify(0);
                return NULL;
            }
        }

        static IndexAccessMethod* getIndex(IndexDescriptor* desc) {
            string type = getAccessMethodName(desc->keyPattern());

            if (IndexNames::HASHED == type) {
                return new HashAccessMethod(desc);
            } else if (IndexNames::GEO_2DSPHERE == type) {
                return new S2AccessMethod(desc);
            } else if (IndexNames::TEXT == type || IndexNames::TEXT_INTERNAL == type) {
                return new FTSAccessMethod(desc);
            } else if (IndexNames::GEO_HAYSTACK == type) {
                return new HaystackAccessMethod(desc);
            } else if ("" == type) {
                return new BtreeAccessMethod(desc);
            } else if (IndexNames::GEO_2D == type) {
                return new TwoDAccessMethod(desc);
            } else {
                cout << "Can't find index for keypattern " << desc->keyPattern() << endl;
                verify(0);
                return NULL;
            }
        }

        // The IndexDetails passed in might refer to a Btree-backed index that is not a proper Btree
        // index.  Each Btree-backed index uses a BtreeCursor.  The BtreeCursor doesn't want the AM
        // for the backed index; it wants to talk Btree directly.  So BtreeCursor always asks for a
        // Btree index.
        static IndexAccessMethod* getBtreeIndex(IndexDescriptor* desc) {
            return new BtreeAccessMethod(desc);
        }
    };

}  // namespace mongo
