// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/expression_reordering.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_cursor.h"
#include "mongo/db/pipeline/document_source_cursor_test_util.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <string_view>

#include <benchmark/benchmark.h>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

// Namespace used for a collection-level change stream.
const NamespaceString kCollectionNss =
    NamespaceString::createNamespaceString_forTest("testDB.testCollection");

// Namespace used for a database-level change stream.
const NamespaceString kDatabaseNss =
    NamespaceString::createNamespaceString_forTest("testDB.$cmd.aggregate");

// Namespace used for a cluster-level change stream.
const NamespaceString kClusterNss =
    NamespaceString::createNamespaceString_forTest("admin.$cmd.aggregate");

// UUID used for collections in oplog entries.
const UUID kUUID = UUID::gen();

const BSONObj kChangeStreamDefault = BSON("$changeStream" << BSONObj());
const BSONObj kChangeStreamShowExpandedEvents =
    BSON("$changeStream" << BSON("showExpandedEvents" << true));

// A constituent stage with inclusion projections.
const BSONObj kProjectInclude =
    BSON("$project" << BSON("operationType" << 1 << "_id" << 1 << "ns" << 1 << "documentKey" << 1));

// A constituent stage with exclusion projections.
const BSONObj kProjectExclude =
    BSON("$project" << BSON("fullDocument" << 0 << "ns" << 0 << "documentKey" << 0));

// A constituent stage to split large change stream events into fragments.
const BSONObj kSplitLargeEvent = BSON("$changeStreamSplitLargeEvent" << BSONObj());

// Builds a match stage that keeps 'value' percent of the entries by matching
// group values in the range [0, value) from a [0, 99] distribution.
BSONObj buildGroupMatch(int value) {
    return BSON("$match" << BSON("fullDocument.group" << BSON("$lt" << value)));
}

// Inject the 'allChangesForCluster' field into the $changeStream stage of the pipeline spec. This
// is used to simulate a cluster-level change stream for benchmarks.
BSONObj injectAllChangesForCluster(const BSONObj& pipeline) {
    BSONArrayBuilder builder;
    for (auto&& elem : pipeline.firstElement().Array()) {
        if (elem.type() == BSONType::object) {
            const auto stageObj = elem.Obj();
            const auto changeStreamElem = stageObj["$changeStream"];
            if (changeStreamElem && changeStreamElem.type() == BSONType::object) {
                BSONObjBuilder changeStreamBuilder;
                changeStreamBuilder.appendElements(changeStreamElem.embeddedObject());
                changeStreamBuilder.append("allChangesForCluster", true);
                builder.append(BSON("$changeStream" << changeStreamBuilder.obj()));
                continue;
            }
        }
        builder.append(elem);
    }
    return BSON("" << builder.arr());
}

// A basic change stream pipeline without any extra stages.
const BSONObj kPipelineBasic = BSON("" << BSON_ARRAY(kChangeStreamDefault));

// A basic change stream pipeline without any extra stages, using 'showExpandedEvents: true'.
const BSONObj kPipelineBasicShowExpanded = BSON("" << BSON_ARRAY(kChangeStreamShowExpandedEvents));

// A change stream pipeline with an inclusion '$project' stage.
const BSONObj kPipelineProjectInclude =
    BSON("" << BSON_ARRAY(kChangeStreamDefault << kProjectInclude));

// A change stream pipeline with an exclusion '$project' stage.
const BSONObj kPipelineProjectExclude =
    BSON("" << BSON_ARRAY(kChangeStreamDefault << kProjectExclude));

// A change stream pipeline with an inclusion '$project' stage, using 'showExpandedEvents: true'.
const BSONObj kPipelineProjectIncludeShowExpanded =
    BSON("" << BSON_ARRAY(kChangeStreamShowExpandedEvents << kProjectInclude));

// A change stream pipeline with an exclusion '$project' stage, using 'showExpandedEvents: true'.
const BSONObj kPipelineProjectExcludeShowExpanded =
    BSON("" << BSON_ARRAY(kChangeStreamShowExpandedEvents << kProjectExclude));

// A change stream pipeline with an additional '$match' stage that keeps only 10% of events.
const BSONObj kPipelineMatch10 =
    BSON("" << BSON_ARRAY(kChangeStreamDefault << buildGroupMatch(10)));

// A change stream pipeline with an additional '$match' stage that keeps only 25% of events.
const BSONObj kPipelineMatch25 =
    BSON("" << BSON_ARRAY(kChangeStreamDefault << buildGroupMatch(25)));

// A change stream pipeline with an additional '$match' stage that keeps only 50% of events.
const BSONObj kPipelineMatch50 =
    BSON("" << BSON_ARRAY(kChangeStreamDefault << buildGroupMatch(50)));

// A change stream pipeline with an additional '$match' stage that keeps 100% of events.
const BSONObj kPipelineMatch100 =
    BSON("" << BSON_ARRAY(kChangeStreamDefault << buildGroupMatch(100)));

// A basic change stream pipeline with an additional '$changeStreamSplitLargeEvent' stage.
const BSONObj kPipelineBasicSplitLargeEvent =
    BSON("" << BSON_ARRAY(kChangeStreamDefault << kSplitLargeEvent));

// A change stream pipeline with an additional '$match' stage that keeps only 10% of events, and an
// inclusion '$project' stage.
const BSONObj kPipelineMatch10ProjectInclude =
    BSON("" << BSON_ARRAY(kChangeStreamDefault << buildGroupMatch(10) << kProjectInclude));

// A change stream pipeline with an additional '$match' stage that keeps only 25% of events, and an
// inclusion '$project' stage.
const BSONObj kPipelineMatch25ProjectInclude =
    BSON("" << BSON_ARRAY(kChangeStreamDefault << buildGroupMatch(25) << kProjectInclude));

// A change stream pipeline with an additional '$match' stage that keeps only 50% of events, and an
// inclusion '$project' stage.
const BSONObj kPipelineMatch50ProjectInclude =
    BSON("" << BSON_ARRAY(kChangeStreamDefault << buildGroupMatch(50) << kProjectInclude));

// A change stream pipeline with an additional '$match' stage that keeps only 100% of events, and an
// inclusion '$project' stage.
const BSONObj kPipelineMatch100ProjectInclude =
    BSON("" << BSON_ARRAY(kChangeStreamDefault << buildGroupMatch(100) << kProjectInclude));


// A change stream pipeline with an additional '$match' stage that keeps only 10% of events, and an
// exclusion '$project' stage.
const BSONObj kPipelineMatch10ProjectExclude =
    BSON("" << BSON_ARRAY(kChangeStreamDefault << buildGroupMatch(10) << kProjectExclude));

// A change stream pipeline with an additional '$match' stage that keeps only 25% of events, and an
// exclusion '$project' stage.
const BSONObj kPipelineMatch25ProjectExclude =
    BSON("" << BSON_ARRAY(kChangeStreamDefault << buildGroupMatch(25) << kProjectExclude));

// A change stream pipeline with an additional '$match' stage that keeps only 50% of events, and an
// exclusion '$project' stage.
const BSONObj kPipelineMatch50ProjectExclude =
    BSON("" << BSON_ARRAY(kChangeStreamDefault << buildGroupMatch(50) << kProjectExclude));

// A change stream pipeline with an additional '$match' stage that keeps only 100% of events, and an
// exclusion '$project' stage.
const BSONObj kPipelineMatch100ProjectExclude =
    BSON("" << BSON_ARRAY(kChangeStreamDefault << buildGroupMatch(100) << kProjectExclude));

// A generator for the '_id' field of the oplog entries. The generated value is a BSON OID.
struct IdGeneratorOID {
    void generate(long long current) {
        oid.init();
    }
    void append(BSONObjBuilder& builder) {
        builder.append("_id"sv, oid);
    }
    OID bson() const {
        return oid;
    }
    OID oid;
};

// A generator for the '_id' field of the oplog entries. The generated value is a BSON string with a
// configurable size prefix string and a variable suffix.
struct IdGeneratorString {
    explicit IdGeneratorString(size_t idSize) : id(idSize, 'X') {}
    void generate(long long current) {
        last = fmt::format("{}-{}", id, current);
    }
    void append(BSONObjBuilder& builder) {
        builder.append("_id"sv, last);
    }
    std::string bson() const {
        return last;
    }
    const std::string id;
    std::string last;
};

// Base class for oplog entry producers. No need for virtual functions here as the benchmark
// execution is templated on the concrete producer type.
struct OplogEntryProducer {
    OplogEntryProducer(const NamespaceString& nss, const UUID& uuid)
        : nss(nss), uuid(uuid), startTime(Date_t::now().asInt64()), increment(0) {}

    const NamespaceString nss;
    const UUID uuid;
    const long long startTime;
    long long increment;
};

// Produces "insert" ("i") oplog entries. The insert events produced here contain documents as
// follows:
// - '_id': value generated from IdGenerator
// - 'group': numeric value between 0 and 99. Used for filtering in `$match` stages.
// - `testField0` .. `testFieldn`: n fields with a string payload of configurable size.
template <typename IdGeneratorType>
struct OplogEntryProducerInserts : OplogEntryProducer {
    OplogEntryProducerInserts(const NamespaceString& nss,
                              const UUID& uuid,
                              IdGeneratorType idGenerator,
                              int numFields,
                              size_t payloadSize)
        : OplogEntryProducer(nss, uuid),
          idGenerator(std::move(idGenerator)),
          payload(payloadSize, 'X'),
          numFields(numFields) {}

    BSONObj operator()() {
        this->idGenerator.generate(increment);
        ++increment;

        BSONObjBuilder builder;
        this->idGenerator.append(builder);
        builder.append("group"sv, increment % 100);
        if (numFields > 0) {
            std::string fieldName;
            for (int i = 0; i < numFields; ++i) {
                fieldName = fmt::format("testField{}", i);
                builder.append(fieldName, payload);
            }
        }
        return BSON("op" << "i" << "ns" << nss.ns_forTest() << "ui" << uuid << "o" << builder.obj()
                         << "o2" << BSON("_id" << this->idGenerator.bson()) << "ts"
                         << Timestamp(static_cast<unsigned long long>(startTime + increment)) << "t"
                         << 1 << "wall" << Date_t::fromMillisSinceEpoch(startTime + increment));
    }

    IdGeneratorType idGenerator;
    const std::string payload;
    const int numFields;
};

// Produces small "insert" oplog entries.
template <typename IdGeneratorType>
struct OplogEntryProducerInsertsSmall : OplogEntryProducerInserts<IdGeneratorType> {
    OplogEntryProducerInsertsSmall(const NamespaceString& nss,
                                   const UUID& uuid,
                                   IdGeneratorType idGenerator)
        : OplogEntryProducerInserts<IdGeneratorType>(
              nss, uuid, std::move(idGenerator), /*numFields=*/0, /*payloadSize=*/0) {}
};

// Produces medium "insert" oplog entries.
template <typename IdGeneratorType>
struct OplogEntryProducerInsertsMedium : OplogEntryProducerInserts<IdGeneratorType> {
    OplogEntryProducerInsertsMedium(const NamespaceString& nss,
                                    const UUID& uuid,
                                    IdGeneratorType idGenerator)
        : OplogEntryProducerInserts<IdGeneratorType>(
              nss, uuid, std::move(idGenerator), /*numFields=*/16, /*payloadSize=*/256) {}
};

// Produces large "insert" oplog entries.
template <typename IdGeneratorType>
struct OplogEntryProducerInsertsLarge : OplogEntryProducerInserts<IdGeneratorType> {
    OplogEntryProducerInsertsLarge(const NamespaceString& nss,
                                   const UUID& uuid,
                                   IdGeneratorType idGenerator)
        : OplogEntryProducerInserts<IdGeneratorType>(
              nss, uuid, std::move(idGenerator), /*numFields=*/40, /*payloadSize=*/8192) {}
};


class ChangeStreamPipelineBenchmark : public benchmark::Fixture {
public:
    void SetUp(benchmark::State& state) final {
        // Turn off logging to not disrupt output with random log messages.
        logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
            mongo::logv2::LogComponent::kDefault, mongo::logv2::LogSeverity::Error());

        // (Generic FCV reference): Test latest FCV behavior.
        serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLatest);

        _scopedGlobalServiceContext =
            std::make_unique<ScopedGlobalServiceContextForTest>(/*shouldSetupTL=*/false);
        _threadClient =
            std::make_unique<ThreadClient>(_scopedGlobalServiceContext->getService(), nullptr);
        _opCtx = (*_threadClient)->makeOperationContext();

        auto service = _scopedGlobalServiceContext->getServiceContext();
        repl::ReplicationCoordinator::set(
            service, std::make_unique<repl::ReplicationCoordinatorMock>(service));
    }

    void TearDown(benchmark::State& state) final {
        // Tear down the pipelines (and the mock PlanExecutor they own) while the OperationContext
        // and ExpressionContext are still alive.
        _execPipeline.reset();
        _logicalPipeline.reset();
        _oplogMatchStage.reset();

        _opCtx.reset();
        _threadClient.reset();
        _scopedGlobalServiceContext.reset();

        logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(
            mongo::logv2::LogComponent::kDefault, mongo::logv2::LogSeverity::Log());
    }

    auto getExpCtx() {
        return _expCtx;
    }

    auto getOpCtx() {
        return _opCtx.get();
    }

    void buildExpressionContext(const NamespaceString& nss) {
        _expCtx = make_intrusive<ExpressionContextForTest>(_opCtx.get(), nss);
    }

protected:
    void buildExecPipeline(const NamespaceString& nss, BSONElement pipelineElement) {
        // Prepare ExpressionContext.
        buildExpressionContext(nss);

        // Register empty change stream specification in the ExpressionContext.
        // This is required because following change stream code relies on it.
        DocumentSourceChangeStreamSpec changeStreamSpec;
        getExpCtx()->setChangeStreamSpec(changeStreamSpec);

        // Simulate a mongod-node change stream.
        getExpCtx()->setInRouter(false);

        // Build pipeline from change stream document sources.
        pipeline_factory::MakePipelineOptions opts{.alreadyOptimized = false,
                                                   .attachCursorSource = false};
        _logicalPipeline = pipeline_factory::makePipeline(pipelineElement, getExpCtx(), opts);

        // The head of the pipeline is the DocumentSourceChangeStreamOplogMatch stage. In a real
        // change stream this stage is popped off and its query becomes the filter of the
        // PlanExecutor behind a $cursor stage (see PipelineD::buildInnerQueryExecutorGeneric).
        // Mirror that here: keep the stage alive so its compiled MatchExpression can be reused, and
        // let 'runBenchmarkWithProducer' attach a $cursor stage backed by a mock PlanExecutor.
        auto poppedHead = _logicalPipeline->popFront();
        _oplogMatchStage = boost::dynamic_pointer_cast<DocumentSourceMatch>(poppedHead);
        invariant(_oplogMatchStage);

        // Enable dynamic match reordering on the oplog match expression. A real change stream oplog
        // COLLSCAN gets this in QueryPlannerAccess::makeCollectionScan; the benchmark bypasses the
        // planner, so enable it here to keep predicate evaluation realistic.
        allowReordering(getOpCtx(), _oplogMatchStage->getMatchExpression());
    }

    template <typename Producer>
    void runBenchmarkWithProducer(benchmark::State& state, Producer&& producer) {
        // Build a mock PlanExecutor that produces oplog entries on the fly and applies the real
        // oplog match predicate to each entry. The predicate is applied through the executor
        // matcher (exec::matcher::matchesBSON), the same path a classic COLLSCAN's Filter stage
        // uses, rather than the aggregation MatchProcessor, so matching includes the same work a
        // real oplog scan performs. Filtered entries are counted here so the benchmark can still
        // report how many events were dropped by the oplog match.
        long long filteredEvents = 0;
        const MatchExpression* oplogFilter = _oplogMatchStage->getMatchExpression();
        test::MockDocumentProvider provider =
            [oplogFilter,
             producer = std::forward<Producer>(producer),
             &filteredEvents]() mutable -> boost::optional<BSONObj> {
            while (true) {
                BSONObj candidate = producer();
                if (exec::matcher::matchesBSON(oplogFilter, candidate)) {
                    return candidate;
                }
                ++filteredEvents;
            }
        };

        auto exec = std::make_unique<test::MockPlanExecutorForOplogScan>(
            getOpCtx(), std::move(provider), Timestamp());
        std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> execWithDeleter(
            std::move(exec).release(), PlanExecutor::Deleter{getOpCtx()});

        auto cursor =
            DocumentSourceCursor::create(std::move(execWithDeleter),
                                         getExpCtx(),
                                         DocumentSourceCursor::CursorType::kRegular,
                                         DocumentSourceCursor::ResumeTrackingType::kOplog);
        cursor->bindCatalogInfo(MultipleCollectionAccessor{},
                                make_intrusive<ShardRoleTransactionResourcesStasherForPipeline>());
        cursor->setCatalogResourceHandle_forTest(
            make_intrusive<test::MockDSCursorCatalogResourceHandle>(std::make_shared<bool>(false)));
        _logicalPipeline->addInitialSource(std::move(cursor));

        _execPipeline = exec::agg::buildPipeline(_logicalPipeline->freeze());

        long long totalSize = 0;
        long long totalEvents = 0;
        for (auto _ : state) {
            auto next = this->_execPipeline->getNextResult();
            // Compute accurate size of the produced events.
            state.PauseTiming();
            if (!next.isEOF() && !next.isPaused()) {
                totalSize += next.getDocument().toBsonWithMetaData().objsize();
                totalEvents++;
            }
            state.ResumeTiming();
        }

        // Total number of change events that were not filtered out.
        state.counters["eventsEmitted"] = totalEvents;

        // Total number of events filtered out by the oplog match inside the mock executor.
        state.counters["eventsFiltered"] = filteredEvents;

        // Average size of the events that were not filtered out.
        if (totalEvents > 0) {
            state.counters["averageEventSize"] = totalSize / static_cast<double>(totalEvents);
        } else {
            state.counters["averageEventSize"] = 0;
        }

        // Number of stages in the change stream pipeline.
        state.counters["pipelineStages"] = _execPipeline->getStages().size();
    }

    boost::intrusive_ptr<DocumentSourceMatch> _oplogMatchStage;
    std::unique_ptr<Pipeline> _logicalPipeline;
    std::unique_ptr<exec::agg::Pipeline> _execPipeline;

private:
    std::unique_ptr<ScopedGlobalServiceContextForTest> _scopedGlobalServiceContext;
    std::unique_ptr<ThreadClient> _threadClient;
    ServiceContext::UniqueOperationContext _opCtx;
    boost::intrusive_ptr<ExpressionContextForTest> _expCtx;
};

}  // namespace

#define ADD_BENCHMARKS_SIZES(name, ns, pipelineSpec)                                               \
    BENCHMARK_F(ChangeStreamPipelineBenchmark, BM_##name##_Small)(benchmark::State & state) {      \
        buildExecPipeline((ns), pipelineSpec.firstElement());                                      \
        runBenchmarkWithProducer(                                                                  \
            state, OplogEntryProducerInsertsSmall(kCollectionNss, kUUID, IdGeneratorOID{}));       \
    }                                                                                              \
    BENCHMARK_F(ChangeStreamPipelineBenchmark, BM_##name##_Medium)(benchmark::State & state) {     \
        buildExecPipeline((ns), pipelineSpec.firstElement());                                      \
        runBenchmarkWithProducer(                                                                  \
            state, OplogEntryProducerInsertsMedium(kCollectionNss, kUUID, IdGeneratorOID{}));      \
    }                                                                                              \
    BENCHMARK_F(ChangeStreamPipelineBenchmark, BM_##name##_Large)(benchmark::State & state) {      \
        buildExecPipeline((ns), pipelineSpec.firstElement());                                      \
        runBenchmarkWithProducer(                                                                  \
            state, OplogEntryProducerInsertsLarge(kCollectionNss, kUUID, IdGeneratorOID{}));       \
    }                                                                                              \
    BENCHMARK_F(ChangeStreamPipelineBenchmark, BM_##name##_LargeID)(benchmark::State & state) {    \
        buildExecPipeline((ns), pipelineSpec.firstElement());                                      \
        runBenchmarkWithProducer(                                                                  \
            state,                                                                                 \
            OplogEntryProducerInsertsSmall(kCollectionNss, kUUID, IdGeneratorString{512 * 1024})); \
    }

#define ADD_BENCHMARKS(name, pipelineSpec)                                      \
    ADD_BENCHMARKS_SIZES(CollectionLevel_##name, kCollectionNss, pipelineSpec); \
    ADD_BENCHMARKS_SIZES(DatabaseLevel_##name, kDatabaseNss, pipelineSpec);     \
    ADD_BENCHMARKS_SIZES(                                                       \
        ClusterLevel_##name, kClusterNss, injectAllChangesForCluster(pipelineSpec));

ADD_BENCHMARKS(Basic, kPipelineBasic);
ADD_BENCHMARKS(BasicShowExpanded, kPipelineBasicShowExpanded);
ADD_BENCHMARKS(BasicSplitLargeEvent, kPipelineBasicSplitLargeEvent);
ADD_BENCHMARKS(ProjectInclude, kPipelineProjectInclude);
ADD_BENCHMARKS(ProjectExclude, kPipelineProjectExclude);
ADD_BENCHMARKS(ProjectIncludeShowExpanded, kPipelineProjectIncludeShowExpanded);
ADD_BENCHMARKS(ProjectExcludeShowExpanded, kPipelineProjectExcludeShowExpanded);
ADD_BENCHMARKS(Match10, kPipelineMatch10);
ADD_BENCHMARKS(Match25, kPipelineMatch25);
ADD_BENCHMARKS(Match50, kPipelineMatch50);
ADD_BENCHMARKS(Match100, kPipelineMatch100);
ADD_BENCHMARKS(Match10ProjectInclude, kPipelineMatch10ProjectInclude);
ADD_BENCHMARKS(Match25ProjectInclude, kPipelineMatch25ProjectInclude);
ADD_BENCHMARKS(Match50ProjectInclude, kPipelineMatch50ProjectInclude);
ADD_BENCHMARKS(Match100ProjectInclude, kPipelineMatch100ProjectInclude);
ADD_BENCHMARKS(Match10ProjectExclude, kPipelineMatch10ProjectExclude);
ADD_BENCHMARKS(Match25ProjectExclude, kPipelineMatch25ProjectExclude);
ADD_BENCHMARKS(Match50ProjectExclude, kPipelineMatch50ProjectExclude);
ADD_BENCHMARKS(Match100ProjectExclude, kPipelineMatch100ProjectExclude);

}  // namespace mongo
