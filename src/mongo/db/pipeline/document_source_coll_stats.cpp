/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/stats/top.h"
#include "mongo/util/time_support.h"

using boost::intrusive_ptr;

namespace mongo {

REGISTER_DOCUMENT_SOURCE(collStats, DocumentSourceCollStats::createFromBson);

const char* DocumentSourceCollStats::getSourceName() const {
    return "$collStats";
}

intrusive_ptr<DocumentSource> DocumentSourceCollStats::createFromBson(
    BSONElement specElem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(40166,
            str::stream() << "$collStats must take a nested object but found: " << specElem,
            specElem.type() == BSONType::Object);
    intrusive_ptr<DocumentSourceCollStats> collStats(new DocumentSourceCollStats(pExpCtx));

    for (const auto& elem : specElem.embeddedObject()) {
        StringData fieldName = elem.fieldNameStringData();

        if (fieldName == "latencyStats") {
            uassert(40167,
                    str::stream() << "latencyStats argument must be an object, but found: " << elem,
                    elem.type() == BSONType::Object);
            collStats->_latencySpecified = true;
        } else {
            uasserted(40168, str::stream() << "unrecognized option to $collStats: " << fieldName);
        }
    }

    return collStats;
}

boost::optional<Document> DocumentSourceCollStats::getNext() {
    if (_finished) {
        return boost::none;
    }

    _finished = true;

    BSONObjBuilder builder;

    builder.appendDate("localTime", jsTime());
    if (_latencySpecified) {
        _mongod->appendLatencyStats(pExpCtx->ns, &builder);
    }

    return Document(builder.obj());
}

bool DocumentSourceCollStats::isValidInitialSource() const {
    return true;
}

Value DocumentSourceCollStats::serialize(bool explain) const {
    if (_latencySpecified) {
        return Value(DOC(getSourceName() << DOC("latencyStats" << Document())));
    }
    return Value(DOC(getSourceName() << Document()));
}

}  // namespace mongo
