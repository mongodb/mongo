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

#include "pch.h"
#include "db/pipeline/document_source.h"

#include "db/jsobj.h"
#include "db/pipeline/document.h"
#include "db/pipeline/expression.h"
#include "db/pipeline/value.h"

namespace mongo {

    const char DocumentSourceProject::projectName[] = "$project";

    DocumentSourceProject::~DocumentSourceProject() {
    }

    DocumentSourceProject::DocumentSourceProject():
        excludeId(false),
        pEO(ExpressionObject::create()) {
    }

    bool DocumentSourceProject::eof() {
        return pSource->eof();
    }

    bool DocumentSourceProject::advance() {
        return pSource->advance();
    }

    intrusive_ptr<Document> DocumentSourceProject::getCurrent() {
        intrusive_ptr<Document> pInDocument(pSource->getCurrent());

        /* create the result document */
        const size_t sizeHint =
            pEO->getSizeHint(pInDocument) + (excludeId ? 0 : 1);
        intrusive_ptr<Document> pResultDocument(Document::create(sizeHint));

        if (!excludeId) {
            intrusive_ptr<const Value> pId(
                pInDocument->getField(Document::idName));
            pResultDocument->addField(Document::idName, pId);
        }

        /* use the ExpressionObject to create the base result */
        pEO->addToDocument(pResultDocument, pInDocument);

        return pResultDocument;
    }

    void DocumentSourceProject::optimize() {
        intrusive_ptr<Expression> pE(pEO->optimize());
        pEO = dynamic_pointer_cast<ExpressionObject>(pE);
    }

    void DocumentSourceProject::sourceToBson(BSONObjBuilder *pBuilder) const {
        BSONObjBuilder insides;
        if (excludeId)
            insides.append(Document::idName, false);
        pEO->documentToBson(&insides, 0);
        pBuilder->append(projectName, insides.done());
    }

    intrusive_ptr<DocumentSourceProject> DocumentSourceProject::create() {
        intrusive_ptr<DocumentSourceProject> pSource(
            new DocumentSourceProject());
        return pSource;
    }

    void DocumentSourceProject::addField(
        const string &fieldName, const intrusive_ptr<Expression> &pExpression) {
        uassert(15960,
                "projection fields must be defined by non-empty expressions",
                pExpression);

        pEO->addField(fieldName, pExpression);
    }

    void DocumentSourceProject::includePath(const string &fieldPath) {
        if (Document::idName.compare(fieldPath) == 0) {
            uassert(15961, str::stream() << projectName <<
                    ":  _id cannot be included once it has been excluded",
                    !excludeId);

            return;
        }

        pEO->includePath(fieldPath);
    }

    void DocumentSourceProject::excludePath(const string &fieldPath) {
        if (Document::idName.compare(fieldPath) == 0) {
            excludeId = true;
            return;
        }

        pEO->excludePath(fieldPath);
    }

    intrusive_ptr<DocumentSource> DocumentSourceProject::createFromBson(
        BSONElement *pBsonElement,
        const intrusive_ptr<ExpressionContext> &pCtx) {
        /* validate */
        uassert(15969, str::stream() << projectName <<
                " specification must be an object",
                pBsonElement->type() == Object);

        /* chain the projection onto the original source */
        intrusive_ptr<DocumentSourceProject> pProject(
            DocumentSourceProject::create());

        /*
          Pull out the $project object.  This should just be a list of
          field inclusion or exclusion specifications.  Note you can't do
          both, except for the case of _id.
         */
        BSONObj projectObj(pBsonElement->Obj());
        BSONObjIterator fieldIterator(projectObj);
        Expression::ObjectCtx objectCtx(
            Expression::ObjectCtx::DOCUMENT_OK);
        while(fieldIterator.more()) {
            BSONElement outFieldElement(fieldIterator.next());
            string outFieldPath(outFieldElement.fieldName());
            string inFieldName(outFieldPath);
            BSONType specType = outFieldElement.type();
            int fieldInclusion = -1;

            switch(specType) {
            case NumberDouble: {
                double inclusion = outFieldElement.numberDouble();
                fieldInclusion = static_cast<int>(inclusion);
                goto IncludeExclude;
            }

            case NumberLong: {
                long long inclusion = outFieldElement.numberLong();
                fieldInclusion = static_cast<int>(inclusion);
                goto IncludeExclude;
            }

            case NumberInt:
                /* just a plain integer include/exclude specification */
                fieldInclusion = outFieldElement.numberInt();

IncludeExclude:
                uassert(15970, str::stream() <<
                        "field inclusion or exclusion specification for \"" <<
                        outFieldPath <<
                        "\" must be true, 1, false, or zero",
                        ((fieldInclusion == 0) || (fieldInclusion == 1)));

                if (fieldInclusion == 0)
                    pProject->excludePath(outFieldPath);
                else 
                    pProject->includePath(outFieldPath);
                break;

            case Bool:
                /* just a plain boolean include/exclude specification */
                fieldInclusion = (outFieldElement.Bool() ? 1 : 0);
                goto IncludeExclude;

            case String:
                /* include a field, with rename */
                fieldInclusion = 1;
                inFieldName = outFieldElement.String();
                pProject->addField(
                    outFieldPath,
                    ExpressionFieldPath::create(
                        Expression::removeFieldPrefix(inFieldName)));
                break;

            case Object: {
                intrusive_ptr<Expression> pDocument(
                    Expression::parseObject(&outFieldElement, &objectCtx));

                /* add The document expression to the projection */
                pProject->addField(outFieldPath, pDocument);
                break;
            }

            default:
                uassert(15971, str::stream() <<
                        "invalid BSON type (" << specType <<
                        ") for " << projectName <<
                        " field " << outFieldPath, false);
            }

        }

        return pProject;
    }
}
