/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/platform/compiler.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/overloaded_visitor.h"  // IWYU pragma: keep

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * This is a wrapper class that holds a const reference to a BatchedCommandRequest and provides a
 * a set of APIs for accessing the contents of the BatchedCommandRequest.
 *
 * Code that uses the BatchWriteCommandRefImpl class must ensure that a BatchWriteCommandRefImpl
 * does not outlive the referred-to BatchedCommandRequest.
 *
 * This class is designed to be used by WriteCommandRef. In general, code outside this file should
 * not use this class directly and instead should prefer to use WriteCommandRef.
 */
class BatchWriteCommandRefImpl {
public:
    explicit BatchWriteCommandRefImpl(const BatchedCommandRequest& request)
        : _ref(std::cref(request)) {}

    /**
     * Returns the referred-to 'const BatchedCommandRequest&'.
     */
    const BatchedCommandRequest& getRequest() const {
        return _ref.get();
    }

    /**
     * Returns the number of write ops in the BatchedCommandRequest. Individual write ops can be
     * referred to using indices from 0 to getNumOps()-1 inclusive.
     */
    size_t getNumOps() const {
        return getRequest().sizeWriteOps();
    }

    /**
     * Visits the referred-to BatchedCommandRequest object.
     */
    template <class Visitor>
    decltype(auto) visitRequest(Visitor&& v) const {
        return std::invoke(std::forward<Visitor>(v), getRequest());
    }

    /**
     * Visit the "data" for the specified insert write op with the specified visitor. 'v' must
     * provide an overload of operator() that supports taking 'const BSONObj&' as its input type.
     *
     * This method will throw an assertion failure if the specified write op is not an insert op.
     */
    template <class Visitor>
    decltype(auto) visitInsertOpData(int index, Visitor&& v) const {
        auto opType = getRequest().getBatchType();
        tassert(10778500, "Expected insert op", opType == BatchedCommandRequest::BatchType_Insert);

        const auto& docs = getRequest().getInsertRequest().getDocuments();
        assertInBounds(index, docs.size());

        return std::invoke(std::forward<Visitor>(v), docs[index]);
    }

    /**
     * Visit the "data" for the specified update write op with the specified visitor. 'v' must
     * provide an overload of operator() that supports taking 'const write_ops::UpdateOpEntry&' as
     * its input type.
     *
     * This method will throw an assertion failure if the specified write op is not an update op.
     */
    template <class Visitor>
    decltype(auto) visitUpdateOpData(int index, Visitor&& v) const {
        auto opType = getRequest().getBatchType();
        tassert(10778501, "Expected update op", opType == BatchedCommandRequest::BatchType_Update);

        const auto& ops = getRequest().getUpdateRequest().getUpdates();
        assertInBounds(index, ops.size());

        return std::invoke(std::forward<Visitor>(v), ops[index]);
    }

    /**
     * Visit the "data" for the specified delete write op with the specified visitor. 'v' must
     * provide an overload of operator() that supports taking 'const write_ops::DeleteOpEntry&' as
     * its input type.
     *
     * This method will throw an assertion failure if the specified write op is not a delete op.
     */
    template <class Visitor>
    decltype(auto) visitDeleteOpData(int index, Visitor&& v) const {
        auto opType = getRequest().getBatchType();
        tassert(10778502, "Expected delete op", opType == BatchedCommandRequest::BatchType_Delete);

        const auto& ops = getRequest().getDeleteRequest().getDeletes();
        assertInBounds(index, ops.size());

        return std::invoke(std::forward<Visitor>(v), ops[index]);
    }

    /**
     * Visit the "data" for the specified update/delete write op with the specified visitor.
     * 'v' must provide overloads of operator() that support the following input types:
     *     const write_ops::UpdateOpEntry&
     *     const write_ops::DeleteOpEntry&
     *
     * This method will throw an assertion failure if the specified write op is not an update op
     * or delete op.
     */
    template <class Visitor>
    decltype(auto) visitUpdateOrDeleteOpData(int index, Visitor&& v) const {
        if (getRequest().getBatchType() == BatchedCommandRequest::BatchType_Update) {
            return visitUpdateOpData(index, std::forward<Visitor>(v));
        }
        if (getRequest().getBatchType() == BatchedCommandRequest::BatchType_Delete) {
            return visitDeleteOpData(index, std::forward<Visitor>(v));
        }
        tasserted(10778503, "Expected update or delete op");
    }

    /**
     * Visit the "data" for the specified write op with the specified visitor. 'v' must provide
     * overloads of operator() that support the following input types:
     *     const BSONObj&
     *     const write_ops::UpdateOpEntry&
     *     const write_ops::DeleteOpEntry&
     */
    template <class Visitor>
    decltype(auto) visitOpData(int index, Visitor&& v) const {
        switch (getRequest().getBatchType()) {
            case BatchedCommandRequest::BatchType_Insert:
                return visitInsertOpData(index, std::forward<Visitor>(v));
            case BatchedCommandRequest::BatchType_Update:
                return visitUpdateOpData(index, std::forward<Visitor>(v));
            case BatchedCommandRequest::BatchType_Delete:
                return visitDeleteOpData(index, std::forward<Visitor>(v));
            default:
                MONGO_UNREACHABLE;
        }
    }

    /**
     * Returns true if the "bypassDocumentValidation" parameter is true, otherwise returns false.
     */
    bool getBypassDocumentValidation() const;

    /**
     * Returns the "bypassEmptyTsReplacement" parameter if present, otherwise returns boost::none.
     */
    const OptionalBool& getBypassEmptyTsReplacement() const;

    /**
     * Returns the "comment" parameter if present, otherwise returns boost::none.
     */
    const boost::optional<IDLAnyTypeOwned>& getComment() const;

    /**
     * BatchedCommandRequest does not allow the "errorOnly" parameter to be specified, so
     * BatchWriteCommandRefImpl::getErrorsOnly() will always return boost::none.
     */
    boost::optional<bool> getErrorsOnly() const;

    /**
     * Returns the "runtimeConstants" parameter if present, otherwise returns boost::none.
     */
    const boost::optional<LegacyRuntimeConstants>& getLegacyRuntimeConstants() const;

    /**
     * Returns the "let" parameter if present, otherwise returns boost::none.
     */
    const boost::optional<BSONObj>& getLet() const;

    /**
     * Returns the "maxTimeMS" parameter if present, otherwise returns boost::none.
     */
    boost::optional<std::int64_t> getMaxTimeMS() const;

    /**
     * Returns a set containing the collection namespace targeted by the write ops in the
     * BatchedCommandRequest.
     */
    std::set<NamespaceString> getNssSet() const;

    /**
     * Returns true if the "ordered" parameter is true, otherwise returns false.
     */
    bool getOrdered() const;

    /**
     * Returns the "stmtId" parameter if present, otherwise returns boost::none.
     */
    boost::optional<std::int32_t> getStmtId() const;

    /**
     * Returns the "stmtIds" parameter if present, otherwise returns boost::none.
     */
    const boost::optional<std::vector<std::int32_t>>& getStmtIds() const;

    /**
     * Returns an estimate of how much space, in bytes, the specified write op would add to a
     * BatchedCommandRequest.
     */
    int estimateOpSizeInBytes(int index) const;

    /**
     * Returns the specified write op's "arrayFilters" field if present, otherwise returns
     * boost::none.
     */
    const boost::optional<std::vector<BSONObj>>& getArrayFilters(int index) const;

    /**
     * Returns the specified write op's "collation" field if present, otherwise returns boost::none.
     */
    const boost::optional<BSONObj>& getCollation(int index) const;

    /**
     * Returns the specified write op's "c" field if present, otherwise returns boost::none.
     */
    const boost::optional<BSONObj>& getConstants(int index) const;

    /**
     * If the specified write op is an insert op, returns the document to be inserted. Otherwise,
     * returns an empty object.
     */
    const BSONObj& getDocument(int index) const;

    /**
     * Returns the specified write op's "q" field if present, otherwise returns an empty object.
     */
    const BSONObj& getFilter(int index) const;

    /**
     * If the specified write op is an update or delete with the "multi" parameter set to true,
     * returns true. Otherwise returns false.
     */
    bool getMulti(int index) const;

    /**
     * Returns the collection namespace targeted by the specified write op.
     */
    const NamespaceString& getNss(int index) const;

    /**
     * Returns the collection UUID targeted by the specified write op.
     */
    const boost::optional<UUID>& getCollectionUUID(int index) const;

    /**
     * Returns the type (insert, update, delete) of the specified write op.
     */
    BatchedCommandRequest::BatchType getOpType(int index) const;

    /**
     * Returns the specified write op's "u" field if present, otherwise returns a default
     * constructed UpdateModification object.
     */
    const write_ops::UpdateModification& getUpdateMods(int index) const;

    /**
     * Returns true if the specified write op is an update op with the "multi" parameter set
     * to true, otherwise returns false.
     */
    bool getUpsert(int index) const;

    /**
     * Returns a BSON representation of the specified write op. Note that this representation
     * may be specific to BatchedCommandRequest and may differ from the representation used by
     * other types of write commands.
     */
    BSONObj toBSON(int index) const;

private:
    void assertInBounds(int index, size_t size) const {
        tassert(10778504,
                "Expected index to be in bounds",
                index < 0 || static_cast<size_t>(index) < size);
    }

    std::reference_wrapper<const BatchedCommandRequest> _ref;
};

/**
 * This is a wrapper class that holds a const reference to a BulkWriteCommandRequest and provides a
 * a set of APIs for accessing the contents of the BulkWriteCommandRequest.
 *
 * Code that uses the BulkWriteCommandRefImpl class must ensure that a BulkWriteCommandRefImpl does
 * not outlive the referred-to BulkWriteCommandRequest.
 *
 * This class is designed to be used by WriteCommandRef. In general, code outside this file should
 * not use this class directly and instead should prefer to use WriteCommandRef.
 */
class BulkWriteCommandRefImpl {
public:
    explicit BulkWriteCommandRefImpl(const BulkWriteCommandRequest& request)
        : _ref(std::cref(request)) {}

    /**
     * Returns the referred-to 'const BulkWriteCommandRequest&'.
     */
    const BulkWriteCommandRequest& getRequest() const {
        return _ref.get();
    }

    /**
     * Returns the number of write ops in the BulkWriteCommandRequest. Individual write ops can be
     * referred to using indices from 0 to getNumOps()-1 inclusive.
     */
    size_t getNumOps() const {
        return getRequest().getOps().size();
    }

    /**
     * Visits the referred-to BulkWriteCommandRequest object.
     */
    template <class Visitor>
    decltype(auto) visitRequest(Visitor&& v) const {
        return std::invoke(std::forward<Visitor>(v), getRequest());
    }

    /**
     * Visit the "data" for the specified insert write op with the specified visitor. 'v' must
     * provide an overload of operator() that supports taking 'const BulkWriteInsertOp&' as its
     * input type.
     *
     * This method will throw an assertion failure if the specified write op is not an insert op.
     */
    template <class Visitor>
    decltype(auto) visitInsertOpData(int index, Visitor&& v) const {
        const auto& ops = getRequest().getOps();
        assertInBounds(index, ops.size());

        auto* opPtr = std::get_if<BulkWriteInsertOp>(&ops[index]);
        tassert(10778505, "Expected insert op", opPtr != nullptr);

        return std::invoke(std::forward<Visitor>(v), *opPtr);
    }

    /**
     * Visit the "data" for the specified update write op with the specified visitor. 'v' must
     * provide an overload of operator() that supports taking 'const BulkWriteUpdateOp&' as its
     * input type.
     *
     * This method will throw an assertion failure if the specified write op is not an update op.
     */
    template <class Visitor>
    decltype(auto) visitUpdateOpData(int index, Visitor&& v) const {
        const auto& ops = getRequest().getOps();
        assertInBounds(index, ops.size());

        auto* opPtr = std::get_if<BulkWriteUpdateOp>(&ops[index]);
        tassert(10778506, "Expected update op", opPtr != nullptr);

        return std::invoke(std::forward<Visitor>(v), *opPtr);
    }

    /**
     * Visit the "data" for the specified delete write op with the specified visitor. 'v' must
     * provide an overload of operator() that supports taking 'const BulkWriteDeleteOp&' as its
     * input type.
     *
     * This method will throw an assertion failure if the specified write op is not a delete op.
     */
    template <class Visitor>
    decltype(auto) visitDeleteOpData(int index, Visitor&& v) const {
        const auto& ops = getRequest().getOps();
        assertInBounds(index, ops.size());

        auto* opPtr = std::get_if<BulkWriteDeleteOp>(&ops[index]);
        tassert(10778507, "Expected delete op", opPtr != nullptr);

        return std::invoke(std::forward<Visitor>(v), *opPtr);
    }

    /**
     * Visit the "data" for the specified update/delete write op with the specified visitor.
     * 'v' must provide overloads of operator() that support the following input types:
     *     const BulkWriteUpdateOp&
     *     const BulkWriteDeleteOp&
     *
     * This method will throw an assertion failure if the specified write op is not an update op
     * or delete op.
     */
    template <class Visitor>
    decltype(auto) visitUpdateOrDeleteOpData(int index, Visitor&& v) const {
        const auto& ops = getRequest().getOps();
        assertInBounds(index, ops.size());

        if (auto* opPtr = std::get_if<BulkWriteUpdateOp>(&ops[index])) {
            return std::invoke(std::forward<Visitor>(v), *opPtr);
        }
        if (auto* opPtr = std::get_if<BulkWriteDeleteOp>(&ops[index])) {
            return std::invoke(std::forward<Visitor>(v), *opPtr);
        }

        tasserted(10778508, "Expected update or delete op");
    }

    /**
     * Visit the "data" for the specified write op with the specified visitor. 'v' must provide
     * overloads of operator() that support the following input types:
     *     const BulkWriteInsertOp&
     *     const BulkWriteUpdateOp&
     *     const BulkWriteDeleteOp&
     */
    template <class Visitor>
    decltype(auto) visitOpData(int index, Visitor&& v) const {
        const auto& ops = getRequest().getOps();
        assertInBounds(index, ops.size());
        return std::visit(std::forward<Visitor>(v), ops[index]);
    }

    /**
     * Returns true if the "bypassDocumentValidation" parameter is true, otherwise returns false.
     */
    bool getBypassDocumentValidation() const;

    /**
     * Returns the "bypassEmptyTsReplacement" parameter if present, otherwise returns boost::none.
     */
    const OptionalBool& getBypassEmptyTsReplacement() const;

    /**
     * Returns the "comment" parameter if present, otherwise returns boost::none.
     */
    const boost::optional<IDLAnyTypeOwned>& getComment() const;

    /**
     * Returns true if the "errorsOnly" parameter is true, otherwise returns false.
     */
    boost::optional<bool> getErrorsOnly() const;

    /**
     * BulkWriteCommandRequest does not allow the "runtimeConstants" parameter to be specified, so
     * BulkWriteCommandRefImpl::getLegacyRuntimeConstants() will always return boost::none.
     */
    const boost::optional<LegacyRuntimeConstants>& getLegacyRuntimeConstants() const;

    /**
     * Returns the "let" parameter if present, otherwise returns boost::none.
     */
    const boost::optional<BSONObj>& getLet() const;

    /**
     * Returns the "maxTimeMS" parameter if present, otherwise returns boost::none.
     */
    boost::optional<std::int64_t> getMaxTimeMS() const;

    /**
     * Returns the set of all collection namespaces targeted by one or more write ops in the
     * BulkWriteCommandRequest.
     */
    std::set<NamespaceString> getNssSet() const;

    /**
     * Returns true if the "ordered" parameter is true, otherwise returns false.
     */
    bool getOrdered() const;

    /**
     * Returns the "stmtId" parameter if present, otherwise returns boost::none.
     */
    boost::optional<std::int32_t> getStmtId() const;

    /**
     * Returns the "stmtIds" parameter if present, otherwise returns boost::none.
     */
    const boost::optional<std::vector<std::int32_t>>& getStmtIds() const;

    /**
     * Returns an estimate of how much space, in bytes, the referred-to write op would add to a
     * BulkWriteCommandRequest.
     */
    int estimateOpSizeInBytes(int index) const;

    /**
     * Returns the specified write op's "arrayFilters" field if present, otherwise returns
     * boost::none.
     */
    const boost::optional<std::vector<BSONObj>>& getArrayFilters(int index) const;

    /**
     * Returns the specified write op's "collation" field if present, otherwise returns boost::none.
     */
    const boost::optional<BSONObj>& getCollation(int index) const;

    /**
     * Returns the specified write op's "constants" field if present, otherwise returns boost::none.
     */
    const boost::optional<BSONObj>& getConstants(int index) const;

    /**
     * If the specified write op is an insert op, returns the document to be inserted. Otherwise,
     * returns an empty object.
     */
    const BSONObj& getDocument(int index) const;

    /**
     * Returns the specified write op's "filter" field if present, otherwise returns an empty
     * object.
     */
    const BSONObj& getFilter(int index) const;

    /**
     * If the specified write op is an update or delete and the "multi" field is set to true,
     * returns true. Otherwise, returns false.
     */
    bool getMulti(int index) const;

    /**
     * Returns the collection namespace targeted by the specified write op.
     */
    const NamespaceString& getNss(int index) const;

    /**
     * Returns the collection UUID targeted by the specified write op.
     */
    const boost::optional<UUID>& getCollectionUUID(int index) const;

    /**
     * Returns the type (insert, update, delete) of the specified write op.
     */
    BatchedCommandRequest::BatchType getOpType(int index) const;

    /**
     * Returns the specified write op's "updateMods" field if present, otherwise returns a default
     * constructed UpdateModification object.
     */
    const write_ops::UpdateModification& getUpdateMods(int index) const;

    /**
     * If the specified write op is an update op with the "upsert" field set to true, returns true.
     * Otherwise, returns false.
     */
    bool getUpsert(int index) const;

    /**
     * Returns a BSON representation of the specified write op. Note that this representation may
     * be specific to BulkWriteCommandRequest and may differ from the representation used by other
     * write commands.
     */
    BSONObj toBSON(int index) const;

private:
    void assertInBounds(int index, size_t size) const {
        tassert(10778509,
                "Expected index to be in bounds",
                index < 0 || static_cast<size_t>(index) < size);
    }

    std::reference_wrapper<const BulkWriteCommandRequest> _ref;
};

/**
 * This is a wrapper class that holds a BatchWriteCommandRefImpl or BulkWriteCommandRefImpl, which
 * in turn holds a const reference to a BatchedCommandRequest or a BatchedCommandRequest object.
 * This class provides a set of APIs for accessing the contents of the BatchedCommandRequest or
 * BulkWriteCommandRequest.
 *
 * Code that uses the WriteCommandRef class must ensure that a WriteCommandRef does not outlive the
 * referred-to BatchedCommandRequest or BulkWriteCommandRequest.
 */
class WriteCommandRef {
public:
    class OpRef;
    class InsertOpRef;
    class UpdateOpRef;
    class DeleteOpRef;

    explicit WriteCommandRef(BatchWriteCommandRefImpl impl) : _impl(std::move(impl)) {}

    explicit WriteCommandRef(BulkWriteCommandRefImpl impl) : _impl(std::move(impl)) {}

    explicit WriteCommandRef(const BatchedCommandRequest& request)
        : _impl(BatchWriteCommandRefImpl{request}) {}

    explicit WriteCommandRef(const BulkWriteCommandRequest& request)
        : _impl(BulkWriteCommandRefImpl{request}) {}

private:
    /**
     * Visit the specific command ref implementation held in '_impl'.
     */
    template <class Visitor>
    decltype(auto) visitImpl(Visitor&& v) const {
        return std::visit(std::forward<Visitor>(v), _impl);
    }

public:
    /**
     * Visits the referred-to request object with the specified visitor. 'v' must provide overloads
     * of operator() that support the following input types:
     *     const BatchedCommandRequest&
     *     const BulkWriteCommandRequest&
     */
    template <class Visitor>
    decltype(auto) visitRequest(Visitor&& v) const {
        return visitImpl(
            [&](auto&& r) -> decltype(auto) { return r.visitRequest(std::forward<Visitor>(v)); });
    }

    /**
     * Returns the number of write ops in the request object that this WriteCommandRef refers to.
     * Individual write ops can be referred to using indices from 0 to getNumOps()-1 inclusive.
     */
    size_t getNumOps() const {
        return visitImpl([&](auto&& r) { return r.getNumOps(); });
    }

    /**
     * Returns a OpRef object for the specified write op in the request object that this
     * WriteCommandRef refers to. The OpRef class (defined below) provides APIs to get
     * information about the write op.
     *
     * If 'index' is not between 0 to getNumOps()-1 inclusive, then this method will throw an
     * assertion failure.
     */
    inline OpRef getOp(int index) const;

    /**
     * Returns true if this WriteCommandRef holds a BatchWriteCommandRefImpl, otherwise returns
     * false.
     */
    bool isBatchWriteCommand() const {
        return holds_alternative<BatchWriteCommandRefImpl>(_impl);
    }

    /**
     * Returns true if this WriteCommandRef holds a BulkWriteCommandRefImpl, otherwise returns
     * false.
     */
    bool isBulkWriteCommand() const {
        return holds_alternative<BulkWriteCommandRefImpl>(_impl);
    }

    /**
     * Returns the BatchWriteCommandRefImpl that this WriteCommandRef refers to. This method will
     * throw an assertion failure if isBatchWriteCommand() is false.
     */
    const BatchedCommandRequest& getBatchedCommandRequest() const {
        tassert(10778510, "Expected BatchWriteCommandRefImpl", isBatchWriteCommand());
        return get<BatchWriteCommandRefImpl>(_impl).getRequest();
    }

    /**
     * Returns the BulkWriteCommandRequest that this WriteCommandRef refers to. This method will
     * throw an assertion failure if isBulkWriteCommand() is false.
     */
    const BulkWriteCommandRequest& getBulkWriteCommandRequest() const {
        tassert(10778511, "Expected BulkWriteCommandRefImpl", isBulkWriteCommand());
        return get<BulkWriteCommandRefImpl>(_impl).getRequest();
    }

    /**
     * Helper methods that forward to the specific implementation held in '_impl'.
     */
    decltype(auto) getBypassDocumentValidation() const {
        return visitImpl(
            [&](auto&& r) -> decltype(auto) { return r.getBypassDocumentValidation(); });
    }
    decltype(auto) getBypassEmptyTsReplacement() const {
        return visitImpl(
            [&](auto&& r) -> decltype(auto) { return r.getBypassEmptyTsReplacement(); });
    }
    decltype(auto) getComment() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getComment(); });
    }
    decltype(auto) getErrorsOnly() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getErrorsOnly(); });
    }
    decltype(auto) getLegacyRuntimeConstants() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getLegacyRuntimeConstants(); });
    }
    decltype(auto) getLet() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getLet(); });
    }
    decltype(auto) getMaxTimeMS() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getMaxTimeMS(); });
    }
    decltype(auto) getNssSet() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getNssSet(); });
    }
    decltype(auto) getOrdered() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getOrdered(); });
    }
    decltype(auto) getStmtId() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getStmtId(); });
    }
    decltype(auto) getStmtIds() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getStmtIds(); });
    }

    /**
     * Comparison ops work by first comparing '_impl.index()', and if '_impl.index()' is the same,
     * then comparing the memory addresses of the request objects.
     */
    friend bool operator==(const WriteCommandRef& lhs, const WriteCommandRef& rhs) {
        return ((lhs <=> rhs) == std::strong_ordering::equal);
    }

    friend std::strong_ordering operator<=>(const WriteCommandRef& lhs,
                                            const WriteCommandRef& rhs) {
        auto indexComparison = (lhs._impl.index() <=> rhs._impl.index());
        if (indexComparison != std::strong_ordering::equal) {
            return indexComparison;
        }
        return std::visit(
            [&](const auto& lhsImpl, const auto& rhsImpl) -> std::strong_ordering {
                if constexpr (std::is_same_v<decltype(lhsImpl), decltype(rhsImpl)>) {
                    return std::compare_three_way{}(&lhsImpl.getRequest(), &rhsImpl.getRequest());
                } else {
                    MONGO_COMPILER_UNREACHABLE;
                }
            },
            lhs._impl,
            rhs._impl);
    }

    /**
     * AbslHashValue() computes the hash using the memory address of the request object.
     */
    template <typename H>
    friend H AbslHashValue(H h, const WriteCommandRef& ref) {
        return ref.visitRequest(
            [&](const auto& request) { return H::combine(std::move(h), &request); });
    }

private:
    using VariantType = std::variant<BatchWriteCommandRefImpl, BulkWriteCommandRefImpl>;

    VariantType _impl;
};

/**
 * This class is similar to WriteCommandRef, but instead of acting as a reference to a whole write
 * command, this class effectively acts as a reference to only one specific write op within a write
 * command. OpRef contains a WriteComamndRef and an integer index. The WriteComamndRef refers to a
 * write command, and the index is used to identify the specific write op within the write command.
 *
 * This class provides a set of APIs for accessing the referred-to write op's data within the
 * request object.
 *
 * Because OpRef contains a WriteCommandRef that refers to a request object, code that uses the
 * OpRef class must ensure that a WriteOpRef does not outlive the referred-to request object.
 */
class WriteCommandRef::OpRef {
public:
    explicit OpRef(WriteCommandRef cmdRef, int index) : _cmdRef(std::move(cmdRef)), _index(index) {
        tassert(10778512,
                "Expected index to be in bounds",
                _index < 0 || static_cast<size_t>(_index) < _cmdRef.getNumOps());
    }

    explicit OpRef(const BatchedCommandRequest& request, int index)
        : OpRef(WriteCommandRef{request}, index) {}

    explicit OpRef(const BulkWriteCommandRequest& request, int index)
        : OpRef(WriteCommandRef{request}, index) {}

    /**
     * Legacy constructors.
     */
    explicit OpRef(const BatchedCommandRequest* request, int index) : OpRef(*request, index) {}

    explicit OpRef(const BulkWriteCommandRequest* request, int index) : OpRef(*request, index) {}

protected:
    /**
     * Visit the specific command ref implementation held in '_cmdRef._impl'.
     */
    template <class Visitor>
    decltype(auto) visitImpl(Visitor&& v) const {
        return _cmdRef.visitImpl(std::forward<Visitor>(v));
    }

public:
    /**
     * Visit the "data" the referred-to insert write op with the specified visitor. 'v' must provide
     * overloads of operator() that support the following input types:
     *     const BSONObj&
     *     const BulkWriteInsertOp&
     *
     * This method will throw an assertion failure if the specified write op is not an insert op.
     */
    template <class Visitor>
    decltype(auto) visitInsertOpData(Visitor&& v) const {
        return visitImpl([&](auto&& r) -> decltype(auto) {
            return r.visitInsertOpData(_index, std::forward<Visitor>(v));
        });
    }

    /**
     * Visit the "data" the referred-to update write op with the specified visitor. 'v' must provide
     * overloads of operator() that support the following input types:
     *     const write_ops::UpdateOpEntry&
     *     const BulkWriteUpdateOp&
     *
     * This method will throw an assertion failure if the specified write op is not an update op.
     */
    template <class Visitor>
    decltype(auto) visitUpdateOpData(Visitor&& v) const {
        return visitImpl([&](auto&& r) -> decltype(auto) {
            return r.visitOpUpdateData(_index, std::forward<Visitor>(v));
        });
    }

    /**
     * Visit the "data" the referred-to delete write op with the specified visitor. 'v' must provide
     * overloads of operator() that support the following input types:
     *     const write_ops::DeleteOpEntry&
     *     const BulkWriteDeleteOp&
     *
     * This method will throw an assertion failure if the specified write op is not a delete op.
     */
    template <class Visitor>
    decltype(auto) visitDeleteOpData(Visitor&& v) const {
        return visitImpl([&](auto&& r) -> decltype(auto) {
            return r.visitDeleteOpData(_index, std::forward<Visitor>(v));
        });
    }

    /**
     * Visit the "data" for the specified update/delete write op with the specified visitor.
     * 'v' must provide overloads of operator() that support the following input types:
     *     const write_ops::UpdateOpEntry&
     *     const write_ops::DeleteOpEntry&
     *     const BulkWriteUpdateOp&
     *     const BulkWriteDeleteOp&
     *
     * This method will throw an assertion failure if the specified write op is not an update op
     * or delete op.
     */
    template <class Visitor>
    decltype(auto) visitUpdateOrDeleteOpData(int index, Visitor&& v) const {
        return visitImpl([&](auto&& r) -> decltype(auto) {
            return r.visitUpdateOrDeleteOpData(_index, std::forward<Visitor>(v));
        });
    }

    /**
     * Visit the "data" the referred-to write op with the specified visitor. 'v' must provide
     * overloads of operator() that support the following input types:
     *     const BSONObj&
     *     const write_ops::UpdateOpEntry&
     *     const write_ops::DeleteOpEntry&
     *     const BulkWriteInsertOp&
     *     const BulkWriteUpdateOp&
     *     const BulkWriteDeleteOp&
     */
    template <class Visitor>
    decltype(auto) visitOpData(Visitor&& v) const {
        return visitImpl([&](auto&& r) -> decltype(auto) {
            return r.visitOpData(_index, std::forward<Visitor>(v));
        });
    }

    /**
     * Returns the WriteCommandRef held by this OpRef.
     */
    const WriteCommandRef& getCommand() const {
        return _cmdRef;
    }

    /**
     * Returns the index of the write op within the command request object. This index will
     * be between 0 and getCommand().getNumOps()-1 inclusive.
     */
    int getIndex() const {
        return _index;
    }

    /**
     * Alias for 'getIndex()'.
     */
    int getItemIndex() const {
        return getIndex();
    }

    /**
     * Returns true if this OpRef holds a BatchWriteCommandRefImpl, otherwise returns false.
     */
    bool isBatchWriteOp() const {
        return _cmdRef.isBatchWriteCommand();
    }

    /**
     * Returns true if this OpRef holds a BulkWriteCommandRefImpl, otherwise returns false.
     */
    bool isBulkWriteOp() const {
        return _cmdRef.isBulkWriteCommand();
    }

    /**
     * Methods for testing if the referred-to write op is an insert, update, or delete.
     */
    decltype(auto) getOpType() const {
        // Call into the specific implementation to determine the type of the write op.
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getOpType(_index); });
    }

    bool isInsertOp() const {
        return getOpType() == BatchedCommandRequest::BatchType_Insert;
    }
    bool isUpdateOp() const {
        return getOpType() == BatchedCommandRequest::BatchType_Update;
    }
    bool isDeleteOp() const {
        return getOpType() == BatchedCommandRequest::BatchType_Delete;
    }

    /**
     * Helper methods for creating an InsertOpRef, UpdateOpRef, or DeleteOpRef from this OpRef.
     * These classes (defined below) are derived from the OpRef class, and they offer additional
     * APIs for accessing the referred-to write op's data.
     */
    inline InsertOpRef getInsertOp() const;
    inline UpdateOpRef getUpdateOp() const;
    inline DeleteOpRef getDeleteOp() const;

    /**
     * Helper methods that forward to the specific implementation held in '_cmdRef._impl'.
     */
    decltype(auto) estimateOpSizeInBytes() const {
        return visitImpl(
            [&](auto&& r) -> decltype(auto) { return r.estimateOpSizeInBytes(_index); });
    }
    decltype(auto) getMulti() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getMulti(_index); });
    }
    decltype(auto) getNss() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getNss(_index); });
    }
    decltype(auto) getCollectionUUID() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getCollectionUUID(_index); });
    }
    decltype(auto) getUpsert() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getUpsert(_index); });
    }
    decltype(auto) toBSON() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.toBSON(_index); });
    }

    /**
     * Comparison ops work by first comparing '_cmdRef', and if '_cmdRef' is the same, then
     * comparing '_index'.
     */
    friend bool operator==(const OpRef& lhs, const OpRef& rhs) = default;
    friend std::strong_ordering operator<=>(const OpRef& lhs, const OpRef& rhs) = default;

    /**
     * AbslHashValue() computes the hash using '_cmdRef' and '_index'.
     */
    template <typename H>
    friend H AbslHashValue(H h, const OpRef& ref) {
        return H::combine(std::move(h), ref._cmdRef, ref._index);
    }

protected:
    WriteCommandRef _cmdRef;
    int _index;
};

inline WriteCommandRef::OpRef WriteCommandRef::getOp(int index) const {
    return WriteCommandRef::OpRef{*this, index};
}

class WriteCommandRef::InsertOpRef : public WriteCommandRef::OpRef {
public:
    explicit InsertOpRef(WriteCommandRef cmdRef, int index) : OpRef(std::move(cmdRef), index) {
        tassert(10778513, "Expected insert op", _cmdRef.getOp(index).isInsertOp());
    }

    /**
     * Helper methods that forward to the specific implementation held in '_cmdRef._impl'.
     */
    decltype(auto) getDocument() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getDocument(_index); });
    }

    friend bool operator==(const InsertOpRef& lhs, const InsertOpRef& rhs) = default;
    friend std::strong_ordering operator<=>(const InsertOpRef& lhs,
                                            const InsertOpRef& rhs) = default;

    template <typename H>
    friend H AbslHashValue(H h, const InsertOpRef& ref) {
        return H::combine(std::move(h), static_cast<const OpRef&>(ref));
    }
};

class WriteCommandRef::UpdateOpRef : public WriteCommandRef::OpRef {
public:
    explicit UpdateOpRef(WriteCommandRef cmdRef, int index) : OpRef(std::move(cmdRef), index) {
        tassert(10778514, "Expected update op", _cmdRef.getOp(index).isUpdateOp());
    }

    /**
     * Helper methods that forward to the specific implementation held in '_cmdRef._impl'.
     */
    decltype(auto) getArrayFilters() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getArrayFilters(_index); });
    }
    decltype(auto) getCollation() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getCollation(_index); });
    }
    decltype(auto) getConstants() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getConstants(_index); });
    }
    decltype(auto) getFilter() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getFilter(_index); });
    }
    decltype(auto) getUpdateMods() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getUpdateMods(_index); });
    }

    friend bool operator==(const UpdateOpRef& lhs, const UpdateOpRef& rhs) = default;
    friend std::strong_ordering operator<=>(const UpdateOpRef& lhs,
                                            const UpdateOpRef& rhs) = default;

    template <typename H>
    friend H AbslHashValue(H h, const UpdateOpRef& ref) {
        return H::combine(std::move(h), static_cast<const OpRef&>(ref));
    }
};

class WriteCommandRef::DeleteOpRef : public WriteCommandRef::OpRef {
public:
    explicit DeleteOpRef(WriteCommandRef cmdRef, int index) : OpRef(std::move(cmdRef), index) {
        tassert(10778515, "Expected delete op", _cmdRef.getOp(index).isDeleteOp());
    }

    /**
     * Helper methods that forward to the specific implementation held in '_cmdRef._impl'.
     */
    decltype(auto) getCollation() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getCollation(_index); });
    }
    decltype(auto) getFilter() const {
        return visitImpl([&](auto&& r) -> decltype(auto) { return r.getFilter(_index); });
    }

    friend bool operator==(const DeleteOpRef& lhs, const DeleteOpRef& rhs) = default;
    friend std::strong_ordering operator<=>(const DeleteOpRef& lhs,
                                            const DeleteOpRef& rhs) = default;

    template <typename H>
    friend H AbslHashValue(H h, const DeleteOpRef& ref) {
        return H::combine(std::move(h), static_cast<const OpRef&>(ref));
    }
};

inline WriteCommandRef::InsertOpRef WriteCommandRef::OpRef::getInsertOp() const {
    return WriteCommandRef::InsertOpRef{getCommand(), _index};
}

inline WriteCommandRef::UpdateOpRef WriteCommandRef::OpRef::getUpdateOp() const {
    return WriteCommandRef::UpdateOpRef{getCommand(), _index};
}

inline WriteCommandRef::DeleteOpRef WriteCommandRef::OpRef::getDeleteOp() const {
    return WriteCommandRef::DeleteOpRef{getCommand(), _index};
}

/**
 * Define 'WriteOpRef' as shorthand for 'WriteCommandRef::OpRef'.
 */
using WriteOpRef = WriteCommandRef::OpRef;
using InsertOpRef = WriteCommandRef::InsertOpRef;
using UpdateOpRef = WriteCommandRef::UpdateOpRef;
using DeleteOpRef = WriteCommandRef::DeleteOpRef;

/**
 * Legacy 'BatchItemRef' type alias.
 */
using BatchItemRef = WriteOpRef;

}  // namespace mongo
