// wiredtiger_util.h

/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <wiredtiger.h>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

    class BSONObjBuilder;

    inline bool wt_keeptxnopen() {
        return false;
    }

    /**
     * converts wiredtiger return codes to mongodb statuses.
     */
    inline Status wtRCToStatus(int retCode, const char* prefix = NULL ) {
        if (MONGO_likely(retCode == 0))
            return Status::OK();


        if ( retCode == WT_ROLLBACK ) {
            //printStackTrace();
            throw WriteConflictException();
        }

        fassert( 28559, retCode != WT_PANIC );

        str::stream s;
        if ( prefix )
            s << prefix << " ";
        s << retCode << ": " << wiredtiger_strerror(retCode);

        if (retCode == EINVAL) {
            return Status(ErrorCodes::BadValue, s);
        }

        // TODO convert specific codes rather than just using UNKNOWN_ERROR for everything.
        return Status(ErrorCodes::UnknownError, s);
    }

    inline void invariantWTOK(int retCode) {
        if (MONGO_likely(retCode == 0))
            return;

        fassertFailedWithStatus(28519, wtRCToStatus(retCode));
    }

    struct WiredTigerItem : public WT_ITEM {
        WiredTigerItem(const void *d, size_t s) {
            data = d;
            size = s;
        }
        WiredTigerItem(const std::string &str) {
            data = str.c_str();
            size = str.size();
        }
        // NOTE: do not call Get() on a temporary.
        // The pointer returned by Get() must not be allowed to live longer than *this.
        WT_ITEM *Get() { return this; }
        const WT_ITEM *Get() const { return this; }
    };

    class WiredTigerUtil {
        MONGO_DISALLOW_COPYING(WiredTigerUtil);
    private:
        WiredTigerUtil();

    public:

        /**
         * Reads contents of table using URI and exports all keys to BSON as string elements.
         * Additional, adds 'uri' field to output document.
         */
        static Status exportTableToBSON(WT_SESSION* s,
                                        const std::string& uri, const std::string& config,
                                        BSONObjBuilder* bob);

        static int64_t getIdentSize(WT_SESSION* s,
                                    const std::string& uri );
    };

    class WiredTigerConfigParser {
        MONGO_DISALLOW_COPYING(WiredTigerConfigParser);
    public:
        WiredTigerConfigParser(const StringData& config) {
            invariantWTOK(wiredtiger_config_parser_open(NULL, config.rawData(), config.size(),
                                                        &_parser));
        }

        WiredTigerConfigParser(const WT_CONFIG_ITEM& nested) {
            invariant(nested.type == WT_CONFIG_ITEM::WT_CONFIG_ITEM_STRUCT);
            invariantWTOK(wiredtiger_config_parser_open(NULL, nested.str, nested.len, &_parser));
        }

        ~WiredTigerConfigParser() {
            invariantWTOK(_parser->close(_parser));
        }

	int next(WT_CONFIG_ITEM* key, WT_CONFIG_ITEM* value) {
            return _parser->next(_parser, key, value);
        }

	int get(const char* key, WT_CONFIG_ITEM* value) {
            return _parser->get(_parser, key, value);
        }

    private:
        WT_CONFIG_PARSER* _parser;
    };

}
