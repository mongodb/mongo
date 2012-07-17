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

    DocumentSourceProject::DocumentSourceProject(
        const intrusive_ptr<ExpressionContext> &pExpCtx):
        DocumentSource(pExpCtx),
        pEO(ExpressionObject::create()),
        _isSimple(true), // set to false in addField
        _wouldBeRemoved(false)
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

    intrusive_ptr<Document> DocumentSourceProject::getCurrent() {
        intrusive_ptr<Document> pInDocument(pSource->getCurrent());

        /* create the result document */
        const size_t sizeHint = pEO->getSizeHint();
        intrusive_ptr<Document> pResultDocument(Document::create(sizeHint));

        /*
          Use the ExpressionObject to create the base result.

          If we're excluding fields at the top level, leave out the _id if
          it is found, because we took care of it above.
        */
        pEO->addToDocument(pResultDocument, pInDocument, /*root=*/pInDocument);

        if (debug && _wouldBeRemoved) {
            // In this case the $project would have been removed in a
            // non-debug build. Make sure that won't change the output.
            if (Document::compare(pResultDocument, pSource->getCurrent()) != 0) {
                log() << "$project removed incorrectly: " << getRaw();
                {
                    BSONObjBuilder printable;
                    pSource->getCurrent()->toBson(&printable);
                    log() << "in:  " << printable.done();
                }
                {
                    BSONObjBuilder printable;
                    pResultDocument->toBson(&printable);
                    log() << "out: " << printable.done();
                }
                verify(false);
            }
        }

        return pResultDocument;
    }

    void DocumentSourceProject::optimize() {
        intrusive_ptr<Expression> pE(pEO->optimize());
        pEO = dynamic_pointer_cast<ExpressionObject>(pE);
    }

    void DocumentSourceProject::sourceToBson(
        BSONObjBuilder *pBuilder, bool explain) const {
        BSONObjBuilder insides;
        pEO->documentToBson(&insides, true);
        pBuilder->append(projectName, insides.done());
    }

    intrusive_ptr<DocumentSource> DocumentSourceProject::createFromBson(
        BSONElement *pBsonElement,
        const intrusive_ptr<ExpressionContext> &pExpCtx) {
        /* validate */
        uassert(15969, str::stream() << projectName <<
                " specification must be an object",
                pBsonElement->type() == Object);

        intrusive_ptr<DocumentSourceProject> pProject(new DocumentSourceProject(pExpCtx));

        BSONObj projectObj(pBsonElement->Obj());
        pProject->_raw = projectObj.getOwned(); // probably not necessary, but better to be safe

        Expression::ObjectCtx objectCtx(
              Expression::ObjectCtx::DOCUMENT_OK
            | Expression::ObjectCtx::TOP_LEVEL
            );

        intrusive_ptr<Expression> parsed = Expression::parseObject(pBsonElement, &objectCtx);
        ExpressionObject* exprObj = dynamic_cast<ExpressionObject*>(parsed.get());
        massert(16402, "parseObject() returned wrong type of Expression", exprObj);
        uassert(16403, "$projection requires at least one output field", exprObj->getFieldCount());
        pProject->pEO = exprObj;

        // TODO simpleness
        pProject->_isSimple = false;
        return pProject;
    }

    DocumentSource::GetDepsReturn DocumentSourceProject::getDependencies(set<string>& deps) const {
        vector<string> path; // empty == top-level
        pEO->addDependencies(deps, &path);
        return EXAUSTIVE;
    }

    void DocumentSourceProject::DependencyRemover::path(
        const string &path, bool include) {
        if (include)
            pTracker->removeDependency(path);
    }

    void DocumentSourceProject::DependencyChecker::path(
        const string &path, bool include) {
        /* if the specified path is included, there's nothing to check */
        if (include)
            return;

        /* if the specified path is excluded, see if it is required */
        intrusive_ptr<const DocumentSource> pSource;
        if (pTracker->getDependency(&pSource, path)) {
            uassert(15984, str::stream() <<
                    "unable to satisfy dependency on " <<
                    FieldPath::getPrefix() <<
                    path << " in pipeline step " <<
                    pSource->getPipelineStep() <<
                    " (" << pSource->getSourceName() << "), because step " <<
                    pThis->getPipelineStep() << " ("
                    << pThis->getSourceName() << ") excludes it",
                    false); // printf() is way easier to read than this crap
        }
    }

    void DocumentSourceProject::manageDependencies(
        const intrusive_ptr<DependencyTracker> &pTracker) {
        
        // the manageDependecies system is currently unused
        return;

#if 0 
        /*
          Look at all the products (inclusions and computed fields) of this
          projection.  For each one that is a dependency, remove it from the
          list of dependencies, because this product will satisfy that
          dependency.
         */
        DependencyRemover dependencyRemover(pTracker);
        pEO->emitPaths(&dependencyRemover);

        /*
          Look at the exclusions of this projection.  If any of them are
          dependencies, inform the user (error/usassert) that the dependency
          can't be satisfied.

          Note we need to do this after the product examination above because
          it is possible for there to be an exclusion field name that matches
          a new computed product field name.  The latter would satisfy the
          dependency.
         */
        DependencyChecker dependencyChecker(pTracker, this);
        pEO->emitPaths(&dependencyChecker);

        /*
          Look at the products of this projection.  For inclusions, add the
          field names to the list of dependencies.  For computed expressions,
          add their dependencies to the list of dependencies.
         */
        pEO->addDependencies(pTracker, this);
#endif
    }

}
