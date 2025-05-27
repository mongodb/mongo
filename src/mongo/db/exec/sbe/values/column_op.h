/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/exec/sbe/values/value.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mongo::sbe::value {
/**
 * The ColumnOpType struct is used as a means to declare various properties about a ColumnOp.
 * These properties include for the moment a flag declaring intrinsic properties of the operation.
 */
struct ColumnOpType {
    enum Flags : uint32_t {
        kNoFlags = 0u,

        // ColumnOp is a monotonic function, i.e. when invoked on a value v1 which is smaller in
        // sort order than v2, the result is guaranteeded to be less or equal (or greater than
        // equal, if monotic descending) than the result of invoking the operation on v2.
        kMonotonic = 1u << 0u,
    };

    constexpr ColumnOpType(Flags flags) : flags(flags) {}

    const Flags flags{kNoFlags};
};

constexpr ColumnOpType::Flags operator~(ColumnOpType::Flags flags) {
    return static_cast<ColumnOpType::Flags>(~static_cast<uint32_t>(flags));
}

constexpr ColumnOpType::Flags operator&(ColumnOpType::Flags lhs, ColumnOpType::Flags rhs) {
    return static_cast<ColumnOpType::Flags>(static_cast<uint32_t>(lhs) &
                                            static_cast<uint32_t>(rhs));
}

constexpr ColumnOpType::Flags operator|(ColumnOpType::Flags lhs, ColumnOpType::Flags rhs) {
    return static_cast<ColumnOpType::Flags>(static_cast<uint32_t>(lhs) |
                                            static_cast<uint32_t>(rhs));
}

constexpr ColumnOpType::Flags operator^(ColumnOpType::Flags lhs, ColumnOpType::Flags rhs) {
    return static_cast<ColumnOpType::Flags>(static_cast<uint32_t>(lhs) ^
                                            static_cast<uint32_t>(rhs));
}

/**
 * Shared base class for all instantiations of the 'ColumnOpFunctor' template.
 */
class ColumnOpFunctorData {
public:
    // 'MethodTable' is a struct of function pointers that serves as the "method table" for
    // a ColumnOp.
    struct MethodTable {
        using ProcessSingleFn = std::pair<TypeTags, Value> (*)(const ColumnOpFunctorData*,
                                                               TypeTags,
                                                               Value);
        using ProcessBatchFn =
            void (*)(const ColumnOpFunctorData*, TypeTags, const Value*, TypeTags*, Value*, size_t);

        constexpr MethodTable(ProcessSingleFn processSingleFn, ProcessBatchFn processBatchFn)
            : processSingleFn(processSingleFn), processBatchFn(processBatchFn) {}

        // processSingleFn() processes a single value.
        const ProcessSingleFn processSingleFn;

        // processBatchFn() processes a variable-size batch of values.
        const ProcessBatchFn processBatchFn;
    };

    ColumnOpFunctorData() = default;
};

/**
 * ColumnOpFunctor provides implementations for all the methods in a ColumnOp's method table.
 *
 * ColumnOpFunctor is a wrapper around the 1 or 2 functors (FuncT1, FuncT2) that are provided
 * to makeColumnOp() or makeColumnOpWithParams().
 *
 * When a single functor is provided to makeColumnOp() or makeColumnOpWithParams(), FuncT1 is used
 * both for processing single values and for processing batches of values (and FuncT2 is ignored).
 *
 * When two functors are provided to makeColumnOp(), FuncT1 is used for processing single values
 * and FuncT2 is used for processing batches of values.
 */
template <ColumnOpType OpType, typename FuncT1, typename FuncT2 = std::nullptr_t>
class ColumnOpFunctor : public ColumnOpFunctorData {
public:
    static constexpr ColumnOpType opType = OpType;

    using SingleFn = FuncT1;
    using BatchFn = std::conditional_t<std::is_same_v<FuncT2, std::monostate>, FuncT1, FuncT2>;

    // Check if 'BatchFn' provides a general batch callback that uses a variable-size batch.
    static constexpr bool hasGeneralBatchFn =
        std::is_invocable_v<const BatchFn&, TypeTags, const Value*, TypeTags*, Value*, size_t>;

    // If 2 functors were provided to makeColumnOp(), the second functor must provide a batch
    // callback.
    static_assert(std::is_same_v<FuncT2, std::monostate> || hasGeneralBatchFn);

    static std::pair<TypeTags, Value> processSingleFn(const ColumnOpFunctorData* cofd,
                                                      TypeTags tag,
                                                      Value val) {
        const ColumnOpFunctor& cof = *static_cast<const ColumnOpFunctor*>(cofd);

        return cof.getSingleFn()(tag, val);
    }

    static void processBatchFn(const ColumnOpFunctorData* cofd,
                               TypeTags inTag,
                               const Value* inVals,
                               TypeTags* outTags,
                               Value* outVals,
                               size_t count) {
        const ColumnOpFunctor& cof = *static_cast<const ColumnOpFunctor*>(cofd);

        if constexpr (hasGeneralBatchFn) {
            cof.getBatchFn()(inTag, inVals, outTags, outVals, count);
        } else {
            for (size_t i = 0; i < count; ++i) {
                std::tie(outTags[i], outVals[i]) = processSingleFn(cofd, inTag, inVals[i]);
            }
        }
    }

    static constexpr auto methodTable = MethodTable{
        &ColumnOpFunctor::processSingleFn,
        &ColumnOpFunctor::processBatchFn,
    };

    ColumnOpFunctor(FuncT1&& functor1, FuncT2&& functor2)
        : functor1(std::forward<FuncT1>(functor1)), functor2(std::forward<FuncT2>(functor2)) {}

    SingleFn& getSingleFn() {
        return functor1;
    }
    const SingleFn& getSingleFn() const {
        return functor1;
    }

    BatchFn& getBatchFn() {
        if constexpr (std::is_null_pointer_v<FuncT2>) {
            return functor1;
        } else {
            return functor2;
        }
    }
    const BatchFn& getBatchFn() const {
        if constexpr (std::is_null_pointer_v<FuncT2>) {
            return functor1;
        } else {
            return functor2;
        }
    }

    FuncT1 functor1;
    FuncT2 functor2;
};

/**
 * This specialization allows for constructing a ColumnOpFunctor with a single arg when only a
 * single input functor was provided to makeColumnOp() or makeColumnOpWithParams().
 */
template <ColumnOpType OpType, typename FuncT>
struct ColumnOpFunctor<OpType, FuncT, std::nullptr_t>
    : public ColumnOpFunctor<OpType, FuncT, std::monostate> {
    using BaseT = ColumnOpFunctor<OpType, FuncT, std::monostate>;

    using MethodTable = typename BaseT::MethodTable;
    using SingleFn = typename BaseT::SingleFn;
    using BatchFn = typename BaseT::BatchFn;

    ColumnOpFunctor(FuncT&& functor) : BaseT(std::forward<FuncT>(functor), {}) {}
};

/**
 * This class serves as the base class for all ColumnOps. The 'ColumnOp' class is not templated
 * and has no static/compile-time knowledge about the specific column operation.
 *
 * ColumnOp consists of a ColumnOpType ('opType'), a ColumnOpFunctor ('cofd'), and a method table
 * ('methodTable').
 *
 * ColumnOp also provides wrapper methods around all the methods in 'methodTable' to make it easy
 * to correctly dispatch to a given method as desired.
 */
struct ColumnOp {
    using MethodTable = ColumnOpFunctorData::MethodTable;

    ColumnOp(ColumnOpType opType, const ColumnOpFunctorData* cofd, const MethodTable& methodTable)
        : opType(opType), cofd(cofd), methodTable(methodTable) {}

    std::pair<TypeTags, Value> processSingle(TypeTags tag, Value val) const {
        return methodTable.processSingleFn(cofd, tag, val);
    }

    void processBatch(TypeTags inTag,
                      const Value* inVals,
                      TypeTags* outTags,
                      Value* outVals,
                      size_t count) const {
        methodTable.processBatchFn(cofd, inTag, inVals, outTags, outVals, count);
    }

    // Helper method that takes care of extracting contiguous chunks of homogeneous values
    // so that they can be invoked by the batch function.
    void processBatch(const TypeTags* inTags,
                      const Value* inVals,
                      TypeTags* outTags,
                      Value* outVals,
                      size_t count) const {
        for (size_t index = 0; index < count;) {
            // Compute the length of the chunk having the same type.
            size_t chunkSize = 1;
            while ((index + chunkSize) < count && inTags[index] == inTags[index + chunkSize]) {
                chunkSize++;
            }
            if (chunkSize == 1) {
                std::tie(outTags[index], outVals[index]) =
                    processSingle(inTags[index], inVals[index]);
            } else {
                processBatch(
                    inTags[index], &inVals[index], &outTags[index], &outVals[index], chunkSize);
            }
            index += chunkSize;
        }
    }

    const ColumnOpType opType;
    const ColumnOpFunctorData* const cofd;
    const MethodTable& methodTable;
};

/**
 * This templated class derived from ColumnOp is the actual class used to represent a specific
 * column operation.
 *
 * This class has holds the ColumnOpFunctor object, and it also makes a copy of the method table
 * when 'copyMethodTable' is true.
 */
template <typename ColumnOpFunctorT, bool copyMethodTable = true>
class ColumnOpInstance : public ColumnOp {
public:
    using FunctorT = ColumnOpFunctorT;

    ColumnOpInstance(ColumnOpType opType, const MethodTable& methodTable, FunctorT&& functor)
        : ColumnOp(opType, &_functor, _methodTable),
          _methodTable(methodTable),
          _functor(std::forward<FunctorT>(functor)) {}

private:
    const MethodTable _methodTable;
    ColumnOpFunctorT _functor;
};

template <typename ColumnOpFunctorT>
class ColumnOpInstance<ColumnOpFunctorT, false> : public ColumnOp {
public:
    using FunctorT = ColumnOpFunctorT;

    ColumnOpInstance(ColumnOpType opType, const MethodTable& methodTable, FunctorT&& functor)
        : ColumnOp(opType, &_functor, methodTable), _functor(std::forward<FunctorT>(functor)) {}

private:
    ColumnOpFunctorT _functor;
};

/**
 * Shared base class for all instantiations of the 'ColumnOpInstanceWithParams' template.
 */
struct ColumnOpWithParams {
    using MethodTable = ColumnOpFunctorData::MethodTable;

    ColumnOpWithParams(ColumnOpType opType, const MethodTable& methodTable)
        : opType(opType), methodTable(methodTable) {}

    const ColumnOpType opType;
    const MethodTable& methodTable;
};

/**
 * The 'ColumnOpInstanceWithParams' class is essentially a "factory" class that can be used to
 * generate ColumnOpInstances via the bindParams() method.
 *
 * This class is used by makeColumnOpWithParams().
 */
template <ColumnOpType OpType, typename FuncT>
class ColumnOpInstanceWithParams : public ColumnOpWithParams {
public:
    using ColumnOpFunctorT = ColumnOpFunctor<OpType, FuncT>;
    using ColumnOpInstanceT = ColumnOpInstance<ColumnOpFunctorT, false>;

    ColumnOpInstanceWithParams()
        : ColumnOpWithParams(OpType, _methodTable), _methodTable(ColumnOpFunctorT::methodTable) {}

    template <typename... Args>
    ColumnOpInstanceT bindParams(Args&&... args) const {
        return ColumnOpInstanceT(
            OpType, _methodTable, ColumnOpFunctorT(FuncT(std::forward<Args>(args)...)));
    }

private:
    const MethodTable _methodTable;
};

/**
 * Helper function for creating a ColumnOp.
 *
 * makeColumnOp() can be invoked in the following ways:
 *
 *    makeColumnOp<opType>({single-value functor})
 *      - When makeColumnOp() is invoked with a single functor arg, a ColumnOp will be created that
 *        uses the arg for processing single values.
 *
 *    makeColumnOp<opType>({single-value functor}, {batch functor})
 *      - When makeColumnOp() is invoked with two functor args, a ColumnOp will be created that uses
 *        the first arg for processing single values and the second arg for processing batches of
 *        values.
 *
 *    makeColumnOp<opType>({all-purpose functor object})
 *      - When makeColumnOp() is invoked with a single functor object arg, a ColumnOp will be
 *        created that uses the functor object for processing single values and also, if the
 *        functor supports it, uses the functor arg for processing batches of values as well.
 *
 */
template <ColumnOpType OpType, typename FuncT>
ColumnOpInstance<ColumnOpFunctor<OpType, FuncT>> makeColumnOp(FuncT&& func) {
    using ColumnOpFunctorT = ColumnOpFunctor<OpType, FuncT>;
    using ColumnOpInstanceT = ColumnOpInstance<ColumnOpFunctorT>;

    return ColumnOpInstanceT(
        OpType, ColumnOpFunctorT::methodTable, ColumnOpFunctorT(std::forward<FuncT>(func)));
}

template <ColumnOpType OpType, typename FuncT1, typename FuncT2>
ColumnOpInstance<ColumnOpFunctor<OpType, FuncT1, FuncT2>> makeColumnOp(FuncT1&& func,
                                                                       FuncT2&& secondParam) {
    using ColumnOpFunctorT = ColumnOpFunctor<OpType, FuncT1, FuncT2>;
    using ColumnOpInstanceT = ColumnOpInstance<ColumnOpFunctorT>;

    return ColumnOpInstanceT(
        OpType,
        ColumnOpFunctorT::methodTable,
        ColumnOpFunctorT(std::forward<FuncT1>(func), std::forward<FuncT2>(secondParam)));
}

/**
 * Helper function for creating a ColumnOpInstanceWithParams.
 *
 * makeColumnOpWithParams() can be invoked like so:
 *
 *    makeColumnOpWithParams<opType, ColumnOpFunctorT>()
 *
 * 'ColumnOpFunctorT' must be a class that defines an 'operator()' method for processing single
 * values. Optionally, 'ColumnOpFunctorT' may also provide 'operator()' methods for processing
 * batches of values.
 *
 * makeColumnOpWithParams() doesn't actually return a ready-to-use ColumnOpInstance, but rather
 * it returns a "factory" object (ColumnOpInstanceWithParams) that can create ColumnOpInstances
 * by calling the bindParams() method.
 */
template <ColumnOpType OpType, typename FuncT>
ColumnOpInstanceWithParams<OpType, FuncT> makeColumnOpWithParams() {
    return ColumnOpInstanceWithParams<OpType, FuncT>();
}
}  // namespace mongo::sbe::value
