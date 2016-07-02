/**
*    Copyright (C) 2011 10gen Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/util/string_map.h"

namespace mongo {

using Parser = DocumentSource::Parser;
using boost::intrusive_ptr;
using std::string;
using std::vector;

DocumentSource::DocumentSource(const intrusive_ptr<ExpressionContext>& pCtx)
    : pSource(NULL), pExpCtx(pCtx) {}

namespace {
// Used to keep track of which DocumentSources are registered under which name.
static StringMap<Parser> parserMap;
}  // namespace

void DocumentSource::registerParser(string name, Parser parser) {
    auto it = parserMap.find(name);
    massert(28707,
            str::stream() << "Duplicate document source (" << name << ") registered.",
            it == parserMap.end());
    parserMap[name] = parser;
}

vector<intrusive_ptr<DocumentSource>> DocumentSource::parse(
    const intrusive_ptr<ExpressionContext> expCtx, BSONObj stageObj) {
    uassert(16435,
            "A pipeline stage specification object must contain exactly one field.",
            stageObj.nFields() == 1);
    BSONElement stageSpec = stageObj.firstElement();
    auto stageName = stageSpec.fieldNameStringData();

    // Get the registered parser and call that.
    auto it = parserMap.find(stageName);

    uassert(16436,
            str::stream() << "Unrecognized pipeline stage name: '" << stageName << "'",
            it != parserMap.end());

    return it->second(stageSpec, expCtx);
}

const char* DocumentSource::getSourceName() const {
    static const char unknown[] = "[UNKNOWN]";
    return unknown;
}

void DocumentSource::setSource(DocumentSource* pTheSource) {
    verify(!isValidInitialSource());
    pSource = pTheSource;
}

intrusive_ptr<DocumentSource> DocumentSource::optimize() {
    return this;
}

void DocumentSource::dispose() {
    if (pSource) {
        pSource->dispose();
    }
}

void DocumentSource::serializeToArray(vector<Value>& array, bool explain) const {
    Value entry = serialize(explain);
    if (!entry.missing()) {
        array.push_back(entry);
    }
}

BSONObjSet DocumentSource::allPrefixes(BSONObj obj) {
    BSONObjSet out;

    BSONObj last = {};
    for (auto&& field : obj) {
        BSONObjBuilder builder(last.objsize() + field.size());
        builder.appendElements(last);
        builder.append(field);
        last = builder.obj();
        out.insert(last);
    }

    return out;
}

BSONObjSet DocumentSource::truncateSortSet(const BSONObjSet& sorts,
                                           const std::set<std::string>& fields) {
    BSONObjSet out;

    for (auto&& sort : sorts) {
        BSONObjBuilder outputSort;

        for (auto&& key : sort) {
            auto keyName = key.fieldNameStringData();

            bool shouldAppend = true;
            for (auto&& field : fields) {
                if (keyName == field || keyName.startsWith(field + '.')) {
                    shouldAppend = false;
                    break;
                }
            }

            if (!shouldAppend) {
                break;
            }

            outputSort.append(key);
        }

        BSONObj outSortObj = outputSort.obj();
        if (!outSortObj.isEmpty()) {
            out.insert(outSortObj);
        }
    }

    return out;
}
}
