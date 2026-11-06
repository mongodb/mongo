// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/write_ops/parsed_find_and_modify.h"

#include "mongo/bson/json.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/query/write_ops/delete_util.h"
#include "mongo/db/update/update_util.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace mongo {
namespace {

using namespace std::literals::string_view_literals;
using query_shape::SerializationOptions;

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("testDb.testColl");

/** Wraps the Value returned by serializeUpdate() into a single-field BSONObj for comparison. */
BSONObj updateValueToBson(const Value& value) {
    return value.wrap(""sv);
}

/**
 * A collator that tracks how many instances are alive. Used to verify that a parsed findAndModify
 * keeps the expression context (and therefore the collator it owns) alive.
 */
class LifetimeTrackingCollator final : public CollatorInterface {
public:
    explicit LifetimeTrackingCollator(int* aliveCount)
        : CollatorInterface(makeSpec()), _aliveCount(aliveCount) {
        ++*_aliveCount;
    }

    ~LifetimeTrackingCollator() override {
        --*_aliveCount;
    }

    std::unique_ptr<CollatorInterface> clone() const override {
        return std::make_unique<LifetimeTrackingCollator>(_aliveCount);
    }

    std::shared_ptr<CollatorInterface> cloneShared() const override {
        return std::make_shared<LifetimeTrackingCollator>(_aliveCount);
    }

    int compare(std::string_view left, std::string_view right) const override {
        return left.compare(right);
    }

    ComparisonKey getComparisonKey(std::string_view stringData) const override {
        return makeComparisonKey(std::string{stringData});
    }

private:
    static Collation makeSpec() {
        std::string_view version{"mock_version"};
        Collation collation(std::string{"mock_lifetime"});
        collation.setBackwards(false);
        collation.setVersion(version);
        return collation;
    }

    int* _aliveCount;
};

class ParsedFindAndModifyTest : public unittest::Test {
public:
    void setUp() final {
        _serviceContext = std::make_unique<QueryTestServiceContext>();
        _opCtx = _serviceContext->makeOperationContext();
    }

    write_ops::FindAndModifyCommandRequest parseCommand(BSONObj command) {
        return write_ops::FindAndModifyCommandRequest::parseOwned(std::move(command));
    }

    boost::intrusive_ptr<ExpressionContext> makeExpressionContext(
        const write_ops::FindAndModifyCommandRequest& request) {
        auto runtimeConstants = request.getLegacyRuntimeConstants()
            ? request.getLegacyRuntimeConstants()
            : boost::optional<LegacyRuntimeConstants>{
                  Variables::generateRuntimeConstants(_opCtx.get())};
        return ExpressionContextBuilder{}
            .opCtx(_opCtx.get())
            .ns(request.getNamespace())
            .letParameters(request.getLet())
            .runtimeConstants(std::move(runtimeConstants))
            .build();
    }

    /**
     * The parsed findAndModify together with the command and write request it borrows. Member
     * order guarantees the borrowed objects outlive 'parsed' (members are destroyed in reverse).
     */
    struct OwnedParse {
        std::unique_ptr<write_ops::FindAndModifyCommandRequest> command;
        std::unique_ptr<UpdateRequest> updateRequest;
        std::unique_ptr<DeleteRequest> deleteRequest;
        std::unique_ptr<ParsedFindAndModify> parsed;
    };

    /** Parses the command, building the write request for its branch as the execution path would.
     */
    OwnedParse parse(BSONObj commandBson) {
        OwnedParse out;
        out.command = std::make_unique<write_ops::FindAndModifyCommandRequest>(
            parseCommand(std::move(commandBson)));
        auto expCtx = makeExpressionContext(*out.command);
        if (out.command->getRemove().value_or(false)) {
            out.deleteRequest = std::make_unique<DeleteRequest>();
            out.deleteRequest->setNsString(out.command->getNamespace());
            delete_util::makeDeleteRequest(
                _opCtx.get(), *out.command, false, out.deleteRequest.get());
            out.parsed = std::make_unique<ParsedFindAndModify>(
                uassertStatusOK(parsed_find_and_modify_command::parse(
                    std::move(expCtx),
                    out.command.get(),
                    out.deleteRequest.get(),
                    makeExtensionsCallback<ExtensionsCallbackNoop>())));
            return out;
        }
        out.updateRequest = std::make_unique<UpdateRequest>();
        out.updateRequest->setNamespaceString(out.command->getNamespace());
        update::makeUpdateRequest(
            _opCtx.get(), *out.command, boost::none /*explain*/, out.updateRequest.get());
        out.parsed = std::make_unique<ParsedFindAndModify>(
            uassertStatusOK(parsed_find_and_modify_command::parse(
                std::move(expCtx),
                out.command.get(),
                out.updateRequest.get(),
                makeExtensionsCallback<ExtensionsCallbackNoop>())));
        return out;
    }

    std::unique_ptr<QueryTestServiceContext> _serviceContext;
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(ParsedFindAndModifyTest, ParsesUpdateBranchAndPreservesOriginalOptions) {
    auto p = parse(fromjson(R"({
        findAndModify: "testColl",
        query: {_id: 42},
        sort: {rank: -1},
        fields: {payload: 1},
        update: {$set: {payload: "updated"}},
        upsert: false,
        new: true,
        let: {threshold: 10},
        $db: "testDb"
    })"));
    const auto& request = *p.command;
    const auto& parsed = *p.parsed;
    ASSERT_FALSE(parsed.isRemove());
    ASSERT(parsed.getParsedUpdate());
    ASSERT_FALSE(parsed.getParsedDelete());
    ASSERT_FALSE(parsed.hasParsedFindCommand());

    // These options have distinct defaults in the internal UpdateRequest. Keep the original
    // command as the source for downstream shape/key logic so absent and explicit values remain
    // distinguishable.
    ASSERT(request.getUpsert());
    ASSERT_FALSE(*request.getUpsert());
    ASSERT(request.getNew());
    ASSERT(*request.getNew());
    ASSERT(request.getSort());
    ASSERT(request.getFields());
    ASSERT(request.getLet());

    ASSERT_BSONOBJ_EQ_AUTO(  // NOLINT
        R"({_id: {$eq: 1}})",
        parsed.serializeQuery(
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions));
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({rank: -1})",
        parsed.serializeSort(
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions));
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({payload: true, _id: true})",
        parsed.serializeProjection(
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions));
}

TEST_F(ParsedFindAndModifyTest, InvalidSortDoesNotFailShapeSerializationOnIdFastPath) {
    auto p = parse(fromjson(R"({
        findAndModify: "testColl",
        query: {_id: 42},
        sort: {a: {$meta: "invalid"}},
        update: {$set: {payload: "updated"}},
        $db: "testDb"
    })"));

    ASSERT_FALSE(p.parsed->hasParsedFindCommand());
    ASSERT_BSONOBJ_EQ(
        BSONObj{},
        p.parsed->serializeSort(
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions));
}

TEST_F(ParsedFindAndModifyTest, PreservesAbsentAndExplicitDefaultOptionsSeparately) {
    auto absentParsed = parse(fromjson(R"({
        findAndModify: "testColl",
        query: {_id: 42},
        update: {$set: {payload: "updated"}},
        $db: "testDb"
    })"));
    ASSERT_FALSE(absentParsed.parsed->getRequest().getUpsert());
    ASSERT_FALSE(absentParsed.parsed->getRequest().getNew());

    auto explicitParsed = parse(fromjson(R"({
        findAndModify: "testColl",
        query: {_id: 42},
        update: {$set: {payload: "updated"}},
        upsert: false,
        new: false,
        $db: "testDb"
    })"));
    ASSERT(explicitParsed.parsed->getRequest().getUpsert());
    ASSERT_FALSE(*explicitParsed.parsed->getRequest().getUpsert());
    ASSERT(explicitParsed.parsed->getRequest().getNew());
    ASSERT_FALSE(*explicitParsed.parsed->getRequest().getNew());
}

TEST_F(ParsedFindAndModifyTest, ParsesRemoveBranchAndPreservesRemoveValue) {
    auto p = parse(fromjson(R"({
        findAndModify: "testColl",
        query: {'items.value': 2},
        sort: {rank: 1},
        fields: {items: {$elemMatch: {value: 2}}},
        remove: true,
        $db: "testDb"
    })"));
    const auto& request = *p.command;
    const auto& parsed = *p.parsed;
    ASSERT_TRUE(parsed.isRemove());
    ASSERT_FALSE(parsed.getParsedUpdate());
    ASSERT(parsed.getParsedDelete());
    ASSERT(parsed.hasParsedFindCommand());
    ASSERT(request.getRemove());
    ASSERT(*request.getRemove());
    ASSERT_FALSE(request.getUpsert());
    ASSERT_FALSE(request.getNew());

    ASSERT_BSONOBJ_EQ_AUTO(
        R"({"items.value": {$eq: 1}})",
        parsed.serializeQuery(
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions));
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({rank: 1})",
        parsed.serializeSort(
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions));
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({items: {$elemMatch: {value: {$eq: 1}}}, _id: true})",
        parsed.serializeProjection(
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions));
}

TEST_F(ParsedFindAndModifyTest, RejectsInvalidCommandBeforeParsingBranch) {
    ASSERT_THROWS_CODE(parse(fromjson(R"({
        findAndModify: "testColl",
        query: {_id: 1},
        remove: true,
        upsert: true,
        $db: "testDb"
    })")),
                       DBException,
                       ErrorCodes::FailedToParse);
}

TEST_F(ParsedFindAndModifyTest, MoveKeepsBorrowedRequestsAccessible) {
    auto p = parse(fromjson(R"({
        findAndModify: "testColl",
        query: {_id: 42},
        update: {$set: {payload: "updated"}},
        $db: "testDb"
    })"));
    ParsedFindAndModify moved(std::move(*p.parsed));

    // The moved wrapper still references the caller's command and request.
    ASSERT(moved.getParsedUpdate());
    ASSERT_EQ(p.command.get(), &moved.getRequest());
    ASSERT_EQ(p.updateRequest.get(), moved.getParsedUpdate()->getRequest());
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({_id: {$eq: 1}})",
        moved.serializeQuery(
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions));
}

TEST_F(ParsedFindAndModifyTest, ParsesWithNoOpExtensionCallback) {
    auto p = parse(fromjson(R"({
        findAndModify: "testColl",
        query: {_id: 42},
        update: {$set: {payload: "updated"}},
        $db: "testDb"
    })"));
    ASSERT(p.parsed->getParsedUpdate());
}

TEST_F(ParsedFindAndModifyTest, ProjectionOnIdFastPathParsesMatcherOnDemand) {
    auto p = parse(fromjson(R"({
        findAndModify: "testColl",
        query: {_id: 42},
        fields: {"items.$": 1},
        remove: true,
        $db: "testDb"
    })"));
    const auto& parsed = *p.parsed;
    ASSERT_FALSE(parsed.hasParsedFindCommand());
    ASSERT_BSONOBJ_EQ_AUTO(
        R"({"items.$": true, _id: true})",
        parsed.serializeProjection(
            query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions));
}

TEST_F(ParsedFindAndModifyTest, ParsesReturnedProjectionAtMostOnceForUpdate) {
    unittest::ServerParameterGuard memoryIntensiveExprLimit(
        "internalQueryMaxMemoryIntensiveExpressions", 1);
    auto p = parse(fromjson(R"({
        findAndModify: "testColl",
        query: {a: 5},
        fields: {out: {$concatArrays: ["$a", "$b"]}},
        update: {$set: {updated: true}},
        $db: "testDb"
    })"));
    const auto& parsed = *p.parsed;

    auto first = parsed.serializeProjection(
        query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    auto second = parsed.serializeProjection(
        query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT_BSONOBJ_EQ(first, second);
}

TEST_F(ParsedFindAndModifyTest, ParsesReturnedProjectionAtMostOnceForRemove) {
    unittest::ServerParameterGuard memoryIntensiveExprLimit(
        "internalQueryMaxMemoryIntensiveExpressions", 1);
    auto p = parse(fromjson(R"({
        findAndModify: "testColl",
        query: {a: 5},
        fields: {out: {$concatArrays: ["$a", "$b"]}},
        remove: true,
        $db: "testDb"
    })"));
    const auto& parsed = *p.parsed;

    auto first = parsed.serializeProjection(
        query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    auto second = parsed.serializeProjection(
        query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT_BSONOBJ_EQ(first, second);
}

/**
 * The parsed findAndModify borrows the caller's command and write request rather than copying
 * them: it and its parsed branch reference the caller-provided objects directly.
 */
TEST_F(ParsedFindAndModifyTest, BorrowsCallerProvidedCommandAndRequest) {
    auto p = parse(fromjson(R"({
        findAndModify: "testColl",
        query: {a: 5},
        update: {$set: {"grades.$[g].score": 5}},
        arrayFilters: [{'g.kind': "ok"}],
        $db: "testDb"
    })"));
    const auto& request = *p.command;
    const auto& parsed = *p.parsed;

    ASSERT_EQ(&request, &parsed.getRequest());
    ASSERT_EQ(p.updateRequest.get(), parsed.getParsedUpdate()->getRequest());

    // Reading the command and serializing the parsed branch reads the borrowed objects.
    ASSERT_BSONOBJ_EQ_AUTO(R"({a: 5})", parsed.getRequest().getQuery());
    ASSERT(parsed.getRequest().getUpdate());
    ASSERT_BSONOBJ_EQ_AUTO(R"({$set: {"grades.$[g].score": 5}})",
                           parsed.getRequest().getUpdate()->getUpdateModifier());
    ASSERT(parsed.getRequest().getArrayFilters());
    ASSERT_EQ(1U, parsed.getRequest().getArrayFilters()->size());
    ASSERT_BSONOBJ_EQ_AUTO(R"({"g.kind": "ok"})", (*parsed.getRequest().getArrayFilters())[0]);

    ASSERT_BSONOBJ_EQ_AUTO(R"({"": {$set: {"grades.$[g].score": 1}}})",
                           updateValueToBson(parsed.serializeUpdate(
                               SerializationOptions::kRepresentativeQueryShapeSerializeOptions)));
    auto filters = parsed.serializeArrayFilters(
        query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT(filters);
    ASSERT_EQ(1U, filters->size());
    ASSERT_BSONOBJ_EQ_AUTO(R"({"g.kind": {$eq: "?"}})", (*filters)[0]);
}

/** The remove branch borrows the caller's delete request the same way. */
TEST_F(ParsedFindAndModifyTest, BorrowsCallerProvidedDeleteRequest) {
    auto p = parse(fromjson(R"({
        findAndModify: "testColl",
        query: {a: 5},
        remove: true,
        $db: "testDb"
    })"));
    const auto& parsed = *p.parsed;

    ASSERT_EQ(p.command.get(), &parsed.getRequest());
    ASSERT_EQ(p.deleteRequest.get(), parsed.getParsedDelete()->getRequest());
    ASSERT_BSONOBJ_EQ_AUTO(R"({a: 5})", parsed.getParsedDelete()->getRequest()->getQuery());
}

/**
 * The returned-document projection is parsed lazily, after 'parse()' has already initialized the
 * system variables referenced by the parsed query. A variable referenced only by the projection
 * (here $$USER_ROLES in 'fields') must still be initialized before serialization: on a shard
 * ('fromRouter'), an initialized system variable serializes as its constant value rather than a
 * variable reference, so missing initialization changes the query shape.
 */
TEST_F(ParsedFindAndModifyTest, InitializesSystemVarsReferencedOnlyByProjection) {
    unittest::ServerParameterGuard userRolesEnabled("enableAccessToUserRoles", true);
    auto request = parseCommand(fromjson(R"({
        findAndModify: "testColl",
        query: {a: 5},
        fields: {out: {$concatArrays: ["$$USER_ROLES", ["$b"]]}},
        update: {$set: {updated: true}},
        $db: "testDb"
    })"));
    auto runtimeConstants = request.getLegacyRuntimeConstants()
        ? request.getLegacyRuntimeConstants()
        : boost::optional<LegacyRuntimeConstants>{
              Variables::generateRuntimeConstants(_opCtx.get())};
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(_opCtx.get())
                      .ns(request.getNamespace())
                      .letParameters(request.getLet())
                      .runtimeConstants(std::move(runtimeConstants))
                      .fromRouter(true)
                      .build();
    UpdateRequest updateRequest;
    updateRequest.setNamespaceString(request.getNamespace());
    update::makeUpdateRequest(_opCtx.get(), request, boost::none /*explain*/, &updateRequest);
    auto parsed = uassertStatusOK(parsed_find_and_modify_command::parse(
        expCtx, &request, &updateRequest, makeExtensionsCallback<ExtensionsCallbackNoop>()));

    // The parsed query does not reference $$USER_ROLES, so it is not initialized yet.
    ASSERT(!expCtx->variables.hasConstantValue(Variables::kUserRolesId));

    auto first = parsed.serializeProjection(
        query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);

    // The projection-only reference must have initialized the variable; serialization emits its
    // constant value rather than a variable reference.
    ASSERT(expCtx->variables.hasConstantValue(Variables::kUserRolesId));
    ASSERT_EQ(std::string::npos, first.jsonString().find("USER_ROLES"));

    // Re-serializing uses the cached projection and stays consistent.
    auto second = parsed.serializeProjection(
        query_shape::SerializationOptions::kRepresentativeQueryShapeSerializeOptions);
    ASSERT_BSONOBJ_EQ(first, second);
}

/**
 * The parsed branch may reference collators owned by the expression context. The parsed
 * findAndModify must retain that context so those references stay valid after the caller drops its
 * own reference.
 */
TEST_F(ParsedFindAndModifyTest, RetainsExpressionContextForParsedBranch) {
    int aliveCollators = 0;
    {
        auto request = parseCommand(fromjson(R"({
            findAndModify: "testColl",
            query: {a: {$in: ["b", "a"]}},
            remove: true,
            $db: "testDb"
        })"));

        auto expCtx = ExpressionContextBuilder{}
                          .opCtx(_opCtx.get())
                          .ns(request.getNamespace())
                          .collator(std::make_unique<LifetimeTrackingCollator>(&aliveCollators))
                          .build();
        ASSERT(aliveCollators >= 1);

        {
            DeleteRequest deleteRequest;
            deleteRequest.setNsString(request.getNamespace());
            delete_util::makeDeleteRequest(_opCtx.get(), request, false, &deleteRequest);
            auto parsed = uassertStatusOK(parsed_find_and_modify_command::parse(
                std::move(expCtx),
                &request,
                &deleteRequest,
                makeExtensionsCallback<ExtensionsCallbackNoop>()));

            // The caller's reference is gone; the parsed object must keep the context (and thus
            // the collator) alive.
            ASSERT(aliveCollators >= 1);

            // Serializing with in-match sorting/deduplication dereferences the collator.
            auto opts = SerializationOptions::kRepresentativeQueryShapeSerializeOptions;
            opts.inMatchExprSortAndDedupElements = true;
            ASSERT_BSONOBJ_EQ_AUTO(R"({a: {$in: ["?"]}})", parsed.serializeQuery(opts));
        }

        // Destroying the parsed object releases the retained context and its collator.
        ASSERT_EQ(0, aliveCollators);
    }
    ASSERT_EQ(0, aliveCollators);
}

TEST_F(ParsedFindAndModifyTest, ProjectionIsNullWhenFieldsAbsentOrEmpty) {
    for (auto cmd : {R"({
            findAndModify: "testColl",
            query: {a: 5},
            update: {$set: {b: 1}},
            $db: "testDb"
        })",
                     R"({
            findAndModify: "testColl",
            query: {a: 5},
            fields: {},
            update: {$set: {b: 1}},
            $db: "testDb"
        })"}) {
        auto p = parse(fromjson(cmd));
        ASSERT_EQ(nullptr, p.parsed->projection());
    }
}

TEST_F(ParsedFindAndModifyTest, ProjectionIsCachedAcrossCalls) {
    auto p = parse(fromjson(R"({
        findAndModify: "testColl",
        query: {a: 5},
        fields: {out: {$concatArrays: ["$a", "$b"]}},
        update: {$set: {b: 1}},
        $db: "testDb"
    })"));

    const auto* first = p.parsed->projection();
    ASSERT(first);
    ASSERT_EQ(first, p.parsed->projection());
}

}  // namespace
}  // namespace mongo
