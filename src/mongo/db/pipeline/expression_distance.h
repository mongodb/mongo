
#pragma once

#include "mongo/db/pipeline/expression.h"

namespace mongo {

// When not using BSON we will use vectors
#ifdef DISTANCE_EXPRESSION_NOT_BSON
#define DISTANCE_EVALUATE_IMPL_PROTO(type)                                                   \
        Value evaluateImpl(                                                                  \
            const std::vector<Value>& vector1,                                               \
            const std::vector<Value>& vector2) const override final;                         
#else
// When using BSON we will get a pointer
#define DISTANCE_EVALUATE_IMPL_PROTO(type)                                                   \
        Value evaluateImpl(                                                                  \
            const type* vector1,                                                             \
            const type* vector2, const size_t size) const override final;
#endif

#define DECLARE_DISTANCE_EXPRESSION(key, class_name, type, error)                            \
    class class_name final                                                                   \
        : public ExpressionDistance<class_name, type, error> {                               \
    public:                                                                                  \
        explicit class_name(const boost::intrusive_ptr<ExpressionContext>& expCtx)           \
                : ExpressionDistance<class_name, type, error>(expCtx) {}                     \
        const char* getOpName() const final {                                                \
            return "$" #key;                                                                 \
        }                                                                                    \
    protected:                                                                               \
        const std::vector<boost::intrusive_ptr<Expression>>& getOperandList() const final {  \
            return vpOperand;                                                                \
        }                                                                                    \
        DISTANCE_EVALUATE_IMPL_PROTO(type)                                                   \
    };

// Template class for defining a distance expression
template <class SubClass, typename T, long ERROR>
class ExpressionDistance : public ExpressionNaryBase<SubClass> {
public:
    explicit ExpressionDistance(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : ExpressionNaryBase<SubClass>(expCtx) {}

    Value evaluate(const Document& root) const final {
        std::string sExpression = getOpName();
        const auto& vpOperand = getOperandList();
        const size_t n = vpOperand.size();

        if (n != 2) {
            uasserted(ERROR,
                str::stream() << sExpression << " only suppports 2 expressions, not " << n);
        }

        const Value& value1 = vpOperand[0]->evaluate(root);
        const Value& value2 = vpOperand[1]->evaluate(root);

#ifndef DISTANCE_EXPRESSION_NOT_BSON
        const BSONBinData& vector1 = value1.getBinData();
        const BSONBinData& vector2 = value2.getBinData();

        if (vector1.length != vector2.length) {
            uasserted(ERROR + 1000L,
                str::stream() << sExpression << " both operands must have the same length.");
        }

        const T* pData1 = (const T*)vector1.data;
        const T* pData2 = (const T*)vector2.data;

        return evaluateImpl(pData1, pData2, vector1.length / sizeof(T));
#else
        if (!value1.isArray()) {
            uasserted(ErrorCodes::FailedToParse,
                str::stream() << sExpression  << " only supports array on 1st expression , not "
                            << typeName(value1.getType()));
        }

        if (!value2.isArray()) {
            uasserted(ErrorCodes::FailedToParse,
                str::stream() << sExpression  << " only supports array on 2nd expression, not "
                            << typeName(value2.getType()));
        }

        const std::vector<Value>& vector1 = value1.getArray();
        const std::vector<Value>& vector2 = value2.getArray();

        if(vector1.size() != vector2.size()){
            uasserted(ErrorCodes::FailedToParse,
                str::stream()  << sExpression << " vectors of different sizes found "
                            << vector1.size() << " " << vector2.size());
        }

        return evaluateImpl(vector1, vector2);
#endif
    }

    bool isAssociative() const final {
        return true;
    }

    bool isCommutative() const final {
        return false;
    }

    virtual const char* getOpName() const = 0;
    
protected:
    virtual const std::vector<boost::intrusive_ptr<Expression>>& getOperandList() const = 0;
#ifndef DISTANCE_EXPRESSION_NOT_BSON
    virtual Value evaluateImpl(const T* p_pData1, const T* p_pData2, const size_t p_uiSize) const = 0;
#else
    virtual Value evaluateImpl(const std::vector<Value>& vector1, const std::vector<Value>& vector2) const = 0;
#endif
};

DECLARE_DISTANCE_EXPRESSION(euclidean, ExpressionEuclidean, float, 9020)
DECLARE_DISTANCE_EXPRESSION(cossim, ExpressionCosineSimilarity, float, 90021)
DECLARE_DISTANCE_EXPRESSION(chi2, ExpressionChi2, float, 90022)
DECLARE_DISTANCE_EXPRESSION(squared_euclidean, ExpressionSquaredEuclidean, float, 90023)
DECLARE_DISTANCE_EXPRESSION(manhattan, ExpressionManhattan, float, 90024)
// Only for benchmarking
DECLARE_DISTANCE_EXPRESSION(no_op, ExpressionNoOp, float, 90025)

}
