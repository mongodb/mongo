#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

    using boost::intrusive_ptr;
    using std::string;
    using std::vector;

    const char DocumentSourceObjectToArray::objectToArrayName[] = "$objectToArray";

    DocumentSourceObjectToArray::DocumentSourceObjectToArray(const intrusive_ptr<ExpressionContext> &pExpCtx, string pathString)
        : DocumentSource(pExpCtx)
        , _otoaFieldPath(pathString)
    {}

    const char *DocumentSourceObjectToArray::getSourceName() const {
        return objectToArrayName;
    }

    boost::optional<Document> DocumentSourceObjectToArray::getNext() {
        pExpCtx->checkForInterrupt();
        boost::optional<Document> input = pSource->getNext();
        if(!input){
            return boost::none;
        }else{
            BSONObjBuilder* builder = new BSONObjBuilder();
            input -> toBson(builder);
            
            _output.reset(Document(builder -> obj()));

            BSONArrayBuilder* arrayBuilder = new BSONArrayBuilder();
            
            Value data = input -> getNestedField(_otoaFieldPath);
            Document dataDoc = data.getDocument();
            FieldIterator ir = dataDoc.fieldIterator();
            while(ir.more()){
                Document::FieldPair fp = ir.next();
                
                Value v = fp.second;
                BSONObjBuilder* valueBuilder = new BSONObjBuilder();
                v.addToBsonObj(valueBuilder, fp.first);
                
                arrayBuilder -> append<BSONObj>(valueBuilder->obj());
            }
            Value rv(arrayBuilder -> arr());
            _output.setNestedField(_otoaFieldPath, rv);
            return _output.peek();
        }
    }

    Value DocumentSourceObjectToArray::serialize(bool explain) const {
        return Value(DOC(getSourceName() << _otoaFieldPath.getPath(true)));
    }
    
    DocumentSource::GetDepsReturn DocumentSourceObjectToArray::getDependencies(DepsTracker* deps) const {
        deps->fields.insert(_otoaFieldPath.getPath(false));
        return SEE_NEXT;
    }

    intrusive_ptr<DocumentSource> DocumentSourceObjectToArray::createFromBson(
            BSONElement elem,
            const intrusive_ptr<ExpressionContext> &pExpCtx) {
        
        /*
         Verify user path
         */
        uassert(20003, str::stream() << "the " << objectToArrayName <<
                " field path must be specified as a string",
                elem.type() == String);
        
        string prefixedPathString(elem.str());
        string pathString(Expression::removeFieldPrefix(prefixedPathString));
        intrusive_ptr<DocumentSourceObjectToArray> otoa(new DocumentSourceObjectToArray(pExpCtx, pathString));
        return otoa;
    }
}
