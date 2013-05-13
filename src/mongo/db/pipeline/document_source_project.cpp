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
 */

#include "mongo/pch.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

    const char DocumentSourceProject::projectName[] = "$project";

    DocumentSourceProject::~DocumentSourceProject() {
    }

    DocumentSourceProject::DocumentSourceProject(const intrusive_ptr<ExpressionContext>& pExpCtx,
                                                 const intrusive_ptr<ExpressionObject>& exprObj)
        : DocumentSource(pExpCtx)
        , pEO(exprObj)
    { }

    const char *DocumentSourceProject::getSourceName() const {
        return projectName;
    }

    bool DocumentSourceProject::eof() {
        return pSource->eof();
    }

    bool DocumentSourceProject::advance() {
        DocumentSource::advance(); // check for interrupts

        return pSource->advance();
    }

    Document DocumentSourceProject::getCurrent() {
        Document pInDocument(pSource->getCurrent());

        /* create the result document */
        const size_t sizeHint = pEO->getSizeHint();
        MutableDocument out (sizeHint);

        /*
          Use the ExpressionObject to create the base result.

          If we're excluding fields at the top level, leave out the _id if
          it is found, because we took care of it above.
        */
        pEO->addToDocument(out, pInDocument, Variables(pInDocument));

#if defined(_DEBUG)
        if (!_simpleProjection.getSpec().isEmpty()) {
            // Make sure we return the same results as Projection class

            BSONObjBuilder inputBuilder;
            pSource->getCurrent()->toBson(&inputBuilder);
            BSONObj input = inputBuilder.done();

            BSONObjBuilder outputBuilder;
            out.peek().toBson(&outputBuilder);
            BSONObj output = outputBuilder.done();

            BSONObj projected = _simpleProjection.transform(input);

            if (projected != output) {
                log() << "$project applied incorrectly: " << getRaw() << endl;
                log() << "input:  " << input << endl;
                log() << "out: " << output << endl;
                log() << "projected: " << projected << endl;
                verify(false); // exits in _DEBUG builds
            }
        }
#endif

        return out.freeze();
    }

    void DocumentSourceProject::optimize() {
        intrusive_ptr<Expression> pE(pEO->optimize());
        pEO = dynamic_pointer_cast<ExpressionObject>(pE);
    }

    void DocumentSourceProject::sourceToBson(BSONObjBuilder* pBuilder, bool explain) const {
        *pBuilder << projectName << pEO->serialize();
    }

    intrusive_ptr<DocumentSource> DocumentSourceProject::createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx) {

        /* validate */
        uassert(15969, str::stream() << projectName <<
                " specification must be an object",
                pBsonElement->type() == Object);

        Expression::ObjectCtx objectCtx(
              Expression::ObjectCtx::DOCUMENT_OK
            | Expression::ObjectCtx::TOP_LEVEL
            | Expression::ObjectCtx::INCLUSION_OK
            );

        intrusive_ptr<Expression> parsed = Expression::parseObject(pBsonElement, &objectCtx);
        ExpressionObject* exprObj = dynamic_cast<ExpressionObject*>(parsed.get());
        massert(16402, "parseObject() returned wrong type of Expression", exprObj);
        uassert(16403, "$projection requires at least one output field", exprObj->getFieldCount());

        intrusive_ptr<DocumentSourceProject> pProject(new DocumentSourceProject(pExpCtx, exprObj));

        BSONObj projectObj = pBsonElement->Obj();
        pProject->_raw = projectObj.getOwned(); // probably not necessary, but better to be safe

#if defined(_DEBUG)
        if (exprObj->isSimple()) {
            set<string> deps;
            vector<string> path;
            exprObj->addDependencies(deps, &path);
            pProject->_simpleProjection.init(depsToProjection(deps));
        }
#endif

        return pProject;
    }

    DocumentSource::GetDepsReturn DocumentSourceProject::getDependencies(set<string>& deps) const {
        vector<string> path; // empty == top-level
        pEO->addDependencies(deps, &path);
        return EXHAUSTIVE;
    }
}
