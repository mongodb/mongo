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
        vpExpression(),
        unwindWhich(-1),
        pNoUnwindDocument(),
        pUnwindArray(),
        pUnwind() {
    }

    bool DocumentSourceProject::eof() {
        /*
          If we're unwinding an array, and there are more elements, then we
          can return more documents.
        */
        if (pUnwind.get() && pUnwind->more())
            return false;

        return pSource->eof();
    }

    bool DocumentSourceProject::advance() {
        if (pUnwind.get() && pUnwind->more()) {
            pUnwindValue = pUnwind->next();
            return true;
        }

        /* release the last document and advance */
        pUnwindValue.reset();
        pUnwind.reset();
        pUnwindArray.reset();
        pNoUnwindDocument.reset();
        return pSource->advance();
    }

    boost::shared_ptr<Document> DocumentSourceProject::getCurrent() {
        if (!pNoUnwindDocument.get()) {
            boost::shared_ptr<Document> pInDocument(pSource->getCurrent());

            /*
              Use the expressions to create a new Document out of the
              source Document
            */
            const size_t n = vFieldName.size();
            pNoUnwindDocument = Document::create(n);
            for(size_t i = 0; i < n; ++i) {
                string outName(vFieldName[i]);
                boost::shared_ptr<Expression> pExpression(vpExpression[i]);

                /* get the value for the field */
                boost::shared_ptr<const Value> pOutValue(
                    pExpression->evaluate(pInDocument));

                /*
                  If we're unwinding this field, and it's an array, then we're
                  going to pick off elements one by one, and make fields of
                  them below.
                */
                if (((int)i == unwindWhich) &&
		    (pOutValue->getType() == Array)) {
                    pUnwindArray = pOutValue;
                    pUnwind = pUnwindArray->getArray();

                    /*
                      The $unwind of an empty array is a NULL value.  If we
                      encounter this, use the non-unwind path, but replace
                      pOutField with a nul.
                    */
                    if (pUnwind->more())
                        pUnwindValue = pUnwind->next();
                    else {
                        pUnwindArray.reset();
                        pUnwind.reset();
                        pOutValue = Value::getNull();
                    }
                }

                /* add the field to the document under construction */
                pNoUnwindDocument->addField(outName, pOutValue);
            }
        }

        /*
          If we're unwinding a field, create an alternate document.  In the
          alternate (clone), replace the unwound array field with the element
          at the appropriate index.
         */
        if (pUnwindArray.get()) {
            /* clone the document with an array we're unwinding */
            boost::shared_ptr<Document> pUnwindDocument(pNoUnwindDocument->clone());

            /* substitute the named field into the prototype document */
            pUnwindDocument->setField(
                (size_t)unwindWhich, vFieldName[unwindWhich], pUnwindValue);

            return pUnwindDocument;
        }

        return pNoUnwindDocument;
    }

    void DocumentSourceProject::optimize() {
	const size_t n = vpExpression.size();
	for(size_t i = 0; i < n; ++i)
	    vpExpression[i] = vpExpression[i]->optimize();
    }

    void DocumentSourceProject::sourceToBson(BSONObjBuilder *pBuilder) const {
	BSONObjBuilder insides;
	
	const size_t n = vFieldName.size();
	for(size_t i = 0; i < n; ++i) {
	    if (i == unwindWhich) {
		BSONObjBuilder unwind;
		vpExpression[i]->addToBsonObj(
		    &unwind, Expression::unwindName, false);
		insides.append(vFieldName[i], unwind.done());
		continue;
	    }

	    vpExpression[i]->addToBsonObj(&insides, vFieldName[i], false);
	}

	pBuilder->append("$project", insides.done());
    }

    boost::shared_ptr<DocumentSourceProject> DocumentSourceProject::create() {
        boost::shared_ptr<DocumentSourceProject> pSource(
            new DocumentSourceProject());
        return pSource;
    }

    void DocumentSourceProject::addField(
        string fieldName, boost::shared_ptr<Expression> pExpression,
	bool unwindArray) {
        assert(fieldName.length()); // CW TODO must be a non-empty string
        assert(pExpression); // CW TODO must be a non-null expression
        assert(!unwindArray || (unwindWhich < 0));
                                             // CW TODO only one unwind allowed

        /* if we're unwinding, remember which field */
        if (unwindArray)
            unwindWhich = vFieldName.size();

        vFieldName.push_back(fieldName);
        vpExpression.push_back(pExpression);
    }

    boost::shared_ptr<DocumentSource> DocumentSourceProject::createFromBson(
	BSONElement *pBsonElement,
	const intrusive_ptr<ExpressionContext> &pCtx) {
        /* validate */
        assert(pBsonElement->type() == Object); // CW TODO user error

        /* chain the projection onto the original source */
        boost::shared_ptr<DocumentSourceProject> pProject(
	    DocumentSourceProject::create());

        /*
          Pull out the $project object.  This should just be a list of
          field inclusion or exclusion specifications.  Note you can't do
          both, except for the case of _id.
         */
        BSONObj projectObj(pBsonElement->Obj());
        BSONObjIterator fieldIterator(projectObj);
	Expression::ObjectCtx objectCtx(
	    Expression::ObjectCtx::UNWIND_OK |
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
                    boost::shared_ptr<Expression> pExpression(
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
                bool hasUnwound = objectCtx.unwindUsed();

                boost::shared_ptr<Expression> pDocument(
                    Expression::parseObject(&outFieldElement, &objectCtx));

                /*
                  Add The document expression to the projection.  We detect
                  an unwound field if we haven't unwound a field yet for this
                  projection, and after parsing find that we have just gotten a
                  $unwind specification.
                 */
                pProject->addField(
                    outFieldName, pDocument,
                    !hasUnwound && objectCtx.unwindUsed());
                break;
            }

            default:
                assert(false); // CW TODO invalid field projection specification
            }

        }

        return pProject;
    }
}
