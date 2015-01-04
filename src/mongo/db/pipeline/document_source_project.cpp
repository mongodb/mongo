/**
 * Copyright 2011 (c) 10gen Inc.
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
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#define MONGO_PCH_WHITELISTED
#include "mongo/platform/basic.h"
#include "mongo/pch.h"
#undef MONGO_PCH_WHITELISTED

#include <boost/smart_ptr.hpp>

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

    using boost::intrusive_ptr;

    const char DocumentSourceProject::projectName[] = "$project";

    DocumentSourceProject::DocumentSourceProject(const intrusive_ptr<ExpressionContext>& pExpCtx,
                                                 const intrusive_ptr<ExpressionObject>& exprObj)
        : DocumentSource(pExpCtx)
        , pEO(exprObj)
    { }

    const char *DocumentSourceProject::getSourceName() const {
        return projectName;
    }

    boost::optional<Document> DocumentSourceProject::getNext() {
        pExpCtx->checkForInterrupt();

        boost::optional<Document> input = pSource->getNext();
        if (!input)
            return boost::none;

        /* create the result document */
        const size_t sizeHint = pEO->getSizeHint();
        MutableDocument out (sizeHint);
        out.copyMetaDataFrom(*input);

        /*
          Use the ExpressionObject to create the base result.

          If we're excluding fields at the top level, leave out the _id if
          it is found, because we took care of it above.
        */
        _variables->setRoot(*input);
        pEO->addToDocument(out, *input, _variables.get());
        _variables->clearRoot();

        return out.freeze();
    }

    void DocumentSourceProject::optimize() {
        intrusive_ptr<Expression> pE(pEO->optimize());
        pEO = boost::dynamic_pointer_cast<ExpressionObject>(pE);
    }

    Value DocumentSourceProject::serialize(bool explain) const {
        return Value(DOC(getSourceName() << pEO->serialize(explain)));
    }

    intrusive_ptr<DocumentSource> DocumentSourceProject::createFromBson(
            BSONElement elem,
            const intrusive_ptr<ExpressionContext> &pExpCtx) {

        /* validate */
        uassert(15969, str::stream() << projectName <<
                " specification must be an object",
                elem.type() == Object);

        Expression::ObjectCtx objectCtx(
              Expression::ObjectCtx::DOCUMENT_OK
            | Expression::ObjectCtx::TOP_LEVEL
            | Expression::ObjectCtx::INCLUSION_OK
            );

        VariablesIdGenerator idGenerator;
        VariablesParseState vps(&idGenerator);
        intrusive_ptr<Expression> parsed = Expression::parseObject(elem.Obj(), &objectCtx, vps);
        ExpressionObject* exprObj = dynamic_cast<ExpressionObject*>(parsed.get());
        massert(16402, "parseObject() returned wrong type of Expression", exprObj);
        uassert(16403, "$projection requires at least one output field", exprObj->getFieldCount());

        intrusive_ptr<DocumentSourceProject> pProject(new DocumentSourceProject(pExpCtx, exprObj));
        pProject->_variables.reset(new Variables(idGenerator.getIdCount()));

        BSONObj projectObj = elem.Obj();
        pProject->_raw = projectObj.getOwned();

        return pProject;
    }

    DocumentSource::GetDepsReturn DocumentSourceProject::getDependencies(DepsTracker* deps) const {
        vector<string> path; // empty == top-level
        pEO->addDependencies(deps, &path);
        return EXHAUSTIVE_FIELDS;
    }
}
