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

    DocumentSourceProject::~DocumentSourceProject() {
    }

    DocumentSourceProject::DocumentSourceProject():
        vpExpression(),
        ravelWhich(-1),
        pNoRavelDocument(),
        pRavelArray(),
        pRavel() {
    }

    bool DocumentSourceProject::eof() {
        /*
          If we're raveling an array, and there are more elements, then we
          can return more documents.
        */
        if (pRavel.get() && pRavel->more())
            return false;

        return pSource->eof();
    }

    bool DocumentSourceProject::advance() {
        if (pRavel.get() && pRavel->more()) {
            pRavelValue = pRavel->next();
            return true;
        }

        /* release the last document and advance */
        pRavelValue.reset();
        pRavel.reset();
        pRavelArray.reset();
        pNoRavelDocument.reset();
        return pSource->advance();
    }

    shared_ptr<Document> DocumentSourceProject::getCurrent() {
        if (!pNoRavelDocument.get()) {
            shared_ptr<Document> pInDocument(pSource->getCurrent());

            /*
              Use the expressions to create a new Document out of the
              source Document
            */
            const size_t n = vFieldName.size();
            pNoRavelDocument = Document::create(n);
            for(size_t i = 0; i < n; ++i) {
                string outName(vFieldName[i]);
                shared_ptr<Expression> pExpression(vpExpression[i]);

                /* get the value for the field */
                shared_ptr<const Value> pOutValue(
                    pExpression->evaluate(pInDocument));

                /*
                  If we're raveling this field, and it's an array, then we're
                  going to pick off elements one by one, and make fields of
                  them below.
                */
                if (((int)i == ravelWhich) && (pOutValue->getType() == Array)) {
                    pRavelArray = pOutValue;
                    pRavel = pRavelArray->getArray();

                    /*
                      The $ravel of an empty array is a nul value.  If we
                      encounter this, use the non-ravel path, but replace
                      pOutField with a nul.
                    */
                    if (pRavel->more())
                        pRavelValue = pRavel->next();
                    else {
                        pRavelArray.reset();
                        pRavel.reset();
                        pOutValue = Value::getNull();
                    }
                }

                /* add the field to the document under construction */
                pNoRavelDocument->addField(outName, pOutValue);
            }
        }

        /*
          If we're raveling a field, create an alternate document.  In the
          alternate (clone), replace the raveled array field with the element
          at the appropriate index.
         */
        if (pRavelArray.get()) {
            /* clone the document with an array we're raveling */
            shared_ptr<Document> pRavelDocument(pNoRavelDocument->clone());

            /* substitute the named field into the prototype document */
            pRavelDocument->setField(
                (size_t)ravelWhich, vFieldName[ravelWhich], pRavelValue);

            return pRavelDocument;
        }

        return pNoRavelDocument;
    }

    void DocumentSourceProject::toBson(BSONObjBuilder *pBuilder) const {
	BSONObjBuilder insides;
	
	const size_t n = vFieldName.size();
	for(size_t i = 0; i < n; ++i)
	    vpExpression[i]->toBson(&insides, vFieldName[i], false);

	pBuilder->append("$project", insides.done());
    }

    shared_ptr<DocumentSourceProject> DocumentSourceProject::create() {
        shared_ptr<DocumentSourceProject> pSource(
            new DocumentSourceProject());
        return pSource;
    }

    void DocumentSourceProject::addField(
        string fieldName, shared_ptr<Expression> pExpression, bool ravelArray) {
        assert(fieldName.length()); // CW TODO must be a non-empty string
        assert(pExpression); // CW TODO must be a non-null expression
        assert(!ravelArray || (ravelWhich < 0));
        // CW TODO only one ravel allowed

        /* if we're raveling, remember which field */
        if (ravelArray)
            ravelWhich = vFieldName.size();

        vFieldName.push_back(fieldName);
        vpExpression.push_back(pExpression);
    }

    shared_ptr<DocumentSource> DocumentSourceProject::createFromBson(
	BSONElement *pBsonElement) {
        /* validate */
        assert(pBsonElement->type() == Object); // CW TODO user error

        /* chain the projection onto the original source */
        shared_ptr<DocumentSourceProject> pProject(
	    DocumentSourceProject::create());

        /*
          Pull out the $project object.  This should just be a list of
          field inclusion or exclusion specifications.  Note you can't do
          both, except for the case of _id.
         */
        BSONObj projectObj(pBsonElement->Obj());
        BSONObjIterator fieldIterator(projectObj);
	Expression::ObjectCtx objectCtx(
	    Expression::ObjectCtx::RAVEL_OK |
	    Expression::ObjectCtx::DOCUMENT_OK);
        while(fieldIterator.more()) {
            BSONElement outFieldElement(fieldIterator.next());
            string outFieldName(outFieldElement.fieldName());
            string inFieldName(outFieldName);
            BSONType specType = outFieldElement.type();
            int fieldInclusion = -1;

            assert(outFieldName.find('.') == outFieldName.npos);
            // CW TODO user error: out field name can't use dot notation

            switch(specType) {
            case NumberDouble: {
                double inclusion = outFieldElement.numberDouble();
                if ((inclusion == 0) || (inclusion == 1))
                    fieldInclusion = (int)inclusion;
                else {
                    assert(false); // CW TODO unimplemented constant expression
                }

                goto AddField;
            }

            case NumberInt:
                /* just a plain integer include/exclude specification */
                fieldInclusion = outFieldElement.numberInt();
                assert((fieldInclusion >= 0) && (fieldInclusion <= 1));
                // CW TODO invalid field projection specification

AddField:
                if (fieldInclusion == 0)
                    assert(false); // CW TODO unimplemented
                else {
                    shared_ptr<Expression> pExpression(
                        ExpressionFieldPath::create(inFieldName));
                    pProject->addField(outFieldName, pExpression, false);
                }
                break;

            case Bool:
                /* just a plain boolean include/exclude specification */
                fieldInclusion = outFieldElement.Bool() ? 1 : 0;
                goto AddField;

            case String:
                /* include a field, with rename */
                fieldInclusion = 1;
                inFieldName = outFieldElement.String();
                goto AddField;

            case Object: {
                bool hasRaveled = objectCtx.ravelUsed();

                shared_ptr<Expression> pDocument(
                    Expression::parseObject(&outFieldElement, &objectCtx));

                /*
                  Add The document expression to the projection.  We detect
                  a raveled field if we haven't raveled a field yet for this
                  projection, and after parsing find that we have just gotten a
                  $ravel specification.
                 */
                pProject->addField(
                    outFieldName, pDocument,
                    !hasRaveled && objectCtx.ravelUsed());
                break;
            }

            default:
                assert(false); // CW TODO invalid field projection specification
            }

        }

        return pProject;
    }
}
