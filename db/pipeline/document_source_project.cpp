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
	pEO(ExpressionObject::create()),
	unwindName(),
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

    intrusive_ptr<Document> DocumentSourceProject::getCurrent() {
        if (!pNoUnwindDocument.get()) {
            intrusive_ptr<Document> pInDocument(pSource->getCurrent());

	    /* create the result document */
	    const size_t sizeHint =
		pEO->getSizeHint(pInDocument) + (excludeId ? 0 : 1);
	    pNoUnwindDocument = Document::create(sizeHint);

	    if (!excludeId) {
		intrusive_ptr<const Value> pId(
		    pInDocument->getField(Document::idName));
		pNoUnwindDocument->addField(Document::idName, pId);
	    }

	    /* use the ExpressionObject to create the base result */
	    pEO->addToDocument(pNoUnwindDocument, pInDocument);

	    /*
	      If we're unwinding this field, and it's an array, then we're
	      going to pick off elements one by one, and make fields of
	      them below.
	    */
	    if (unwindName.length() &&
		((unwindWhich = pNoUnwindDocument->getFieldIndex(unwindName)) <
		 pNoUnwindDocument->getFieldCount())) {
		Document::FieldPair outPair(
		    pNoUnwindDocument->getField(unwindWhich));
		if (outPair.second->getType() == Array) {
		    pUnwindArray = outPair.second;
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
			pNoUnwindDocument->setField(
			    unwindWhich, unwindName, Value::getNull());
                    }
		}
	    }
	}


        /*
          If we're unwinding a field, create an alternate document.  In the
          alternate (clone), replace the unwound array field with the element
          at the appropriate index.
         */
        if (pUnwindArray.get()) {
            /* clone the document with an array we're unwinding */
            intrusive_ptr<Document> pUnwindDocument(pNoUnwindDocument->clone());

            /* substitute the named field into the prototype document */
            pUnwindDocument->setField(unwindWhich, unwindName, pUnwindValue);

            return pUnwindDocument;
        }

        return pNoUnwindDocument;
    }

    void DocumentSourceProject::optimize() {
	shared_ptr<Expression> pE(pEO->optimize());
	pEO = dynamic_pointer_cast<ExpressionObject>(pE);
    }

    void DocumentSourceProject::sourceToBson(BSONObjBuilder *pBuilder) const {
	BSONObjBuilder insides;
	if (excludeId)
	    insides.append(Document::idName, false);
	pEO->documentToBson(&insides, unwindName, 0);
	pBuilder->append("$project", insides.done());
    }

    shared_ptr<DocumentSourceProject> DocumentSourceProject::create() {
        shared_ptr<DocumentSourceProject> pSource(
            new DocumentSourceProject());
        return pSource;
    }

    void DocumentSourceProject::addField(
        const string &fieldName, const shared_ptr<Expression> &pExpression,
	bool unwindArray) {
        assert(pExpression); // CW TODO must be a non-null expression

        /* if we're unwinding, remember which field */
        if (unwindArray) {
	    assert(!unwindName.length());
	                           // CW TODO ERROR: only one unwind allowed

	    FieldPath fp(fieldName);
	    assert(fp.getPathLength() == 1);
	                   // CW TODO ERROR: can only unwind a top-level field

	    /* remember which field */
	    unwindName = fieldName;
	}

	pEO->addField(fieldName, pExpression);
    }

    void DocumentSourceProject::includePath(const string &fieldPath) {
	if (Document::idName.compare(fieldPath) == 0) {
	    assert(!excludeId); // included by default, don't allow switching
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

    shared_ptr<DocumentSource> DocumentSourceProject::createFromBson(
	BSONElement *pBsonElement,
	const intrusive_ptr<ExpressionContext> &pCtx) {
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
	    Expression::ObjectCtx::UNWIND_OK |
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
                if ((inclusion == 0) || (inclusion == 1))
                    fieldInclusion = (int)inclusion;
                else {
                    assert(false); // CW TODO unimplemented constant expression
                }

                goto IncludeExclude;
            }

            case NumberInt:
                /* just a plain integer include/exclude specification */
                fieldInclusion = outFieldElement.numberInt();
                assert((fieldInclusion >= 0) && (fieldInclusion <= 1));
                // CW TODO invalid field projection specification

IncludeExclude:
                if (fieldInclusion == 0)
		    pProject->excludePath(outFieldPath);
                else 
                    pProject->includePath(outFieldPath);
                break;

            case Bool:
                /* just a plain boolean include/exclude specification */
                fieldInclusion = outFieldElement.Bool() ? 1 : 0;
                goto IncludeExclude;

            case String:
                /* include a field, with rename */
                fieldInclusion = 1;
                inFieldName = outFieldElement.String();
		pProject->addField(
		    outFieldPath,
		    ExpressionFieldPath::create(
			Expression::removeFieldPrefix(inFieldName)), false);
		break;

            case Object: {
                bool hasUnwound = objectCtx.unwindUsed();

                shared_ptr<Expression> pDocument(
                    Expression::parseObject(&outFieldElement, &objectCtx));

                /*
                  Add The document expression to the projection.  We detect
                  an unwound field if we haven't unwound a field yet for this
                  projection, and after parsing find that we have just gotten a
                  $unwind specification.
                 */
                pProject->addField(
                    outFieldPath, pDocument,
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
