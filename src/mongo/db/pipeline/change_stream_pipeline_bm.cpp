/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/agg/queue_stage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_queue.h"
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

#include <benchmark/benchmark.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

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
        builder.append("_id"_sd, oid);
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
        builder.append("_id"_sd, last);
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
        builder.append("group"_sd, increment % 100);
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
              nss, uuid, std::move(idGenerator), 0 /* numFields */, 0 /* payloadSize */) {}
};

// Produces medium "insert" oplog entries.
template <typename IdGeneratorType>
struct OplogEntryProducerInsertsMedium : OplogEntryProducerInserts<IdGeneratorType> {
    OplogEntryProducerInsertsMedium(const NamespaceString& nss,
                                    const UUID& uuid,
                                    IdGeneratorType idGenerator)
        : OplogEntryProducerInserts<IdGeneratorType>(
              nss, uuid, std::move(idGenerator), 16 /* numFields */, 256 /* payloadSize */) {}
};

// Produces large "insert" oplog entries.
template <typename IdGeneratorType>
struct OplogEntryProducerInsertsLarge : OplogEntryProducerInserts<IdGeneratorType> {
    OplogEntryProducerInsertsLarge(const NamespaceString& nss,
                                   const UUID& uuid,
                                   IdGeneratorType idGenerator)
        : OplogEntryProducerInserts<IdGeneratorType>(
              nss, uuid, std::move(idGenerator), 40 /* numFields */, 8192 /* payloadSize */) {}
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
            std::make_unique<ScopedGlobalServiceContextForTest>(false /* shouldSetupTL */);
        _threadClient =
            std::make_unique<ThreadClient>(_scopedGlobalServiceContext->getService(), nullptr);
        _opCtx = (*_threadClient)->makeOperationContext();

        auto service = _scopedGlobalServiceContext->getServiceContext();
        repl::ReplicationCoordinator::set(
            service, std::make_unique<repl::ReplicationCoordinatorMock>(service));
    }

    void TearDown(benchmark::State& state) final {
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
        auto pipeline = pipeline_factory::makePipeline(pipelineElement, getExpCtx(), opts);


        // Pop the head of the pipeline, which is the DocumentSourceChangeStreamOplogMatch stage.
        // This stage cannot be converted to an exec::agg::State. In real world change streams,
        // this stage is later replaced with a CursorStage. The CursorStage in real world change
        // streams contains an aggregation pipeline which performs a collscan on the oplog with a
        // large match expression.
        // In benchmarks, this whole machinery is replaced with a QueueStage as the pipeline's input
        // stage. The QueueStage produces the input oplog entries using the generators above. After
        // the QueueStage, a match stage is added to simulate the filtering done by the oplog match
        // stage in real world change streams.
        auto* originalMatchStage = dynamic_cast<DocumentSourceMatch*>(pipeline->peekFront());
        invariant(originalMatchStage);
        BSONObj oplogFilter = originalMatchStage->getQuery().getOwned();
        pipeline->popFront();

        // Create a new match stage that will run the oplog filter instead. This is our way of
        // simulating the oplog match filtering from a non-cursor input.
        auto matchSource = DocumentSourceMatch::create(oplogFilter, getExpCtx());
        pipeline->addInitialSource(matchSource);

        // Add a queue stage as the pipeline's input stage. This stage will produce oplog entries on
        // the fly for the following stages.
        auto queueSource = DocumentSourceQueue::create(getExpCtx());
        pipeline->addInitialSource(queueSource.get());

        _execPipeline = exec::agg::buildPipeline(pipeline->freeze());
        _queueStage =
            dynamic_cast<exec::agg::QueueStage*>(_execPipeline->getStages().front().get());
        invariant(_queueStage);
    }

    template <typename Producer>
    void runBenchmarkWithProducer(benchmark::State& state, Producer&& producer) {
        long long totalSize = 0;
        long long totalEvents = 0;
        long long filteredEvents = 0;
        this->_queueStage->emplace_back(Document(producer()));

        for (auto _ : state) {
            auto next = this->_execPipeline->getNextResult();
            // Compute accurate size of the produced events and produce the next oplog entry.
            state.PauseTiming();
            if (!next.isEOF() && !next.isPaused()) {
                totalSize += next.getDocument().toBsonWithMetaData().objsize();
                totalEvents++;
            } else {
                filteredEvents++;
            }
            this->_queueStage->emplace_back(Document(producer()));
            state.ResumeTiming();
        }

        // Total number of change events that were not filtered out.
        state.counters["eventsEmitted"] = totalEvents;

        // Total number of events filtered out.
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

    std::unique_ptr<exec::agg::Pipeline> _execPipeline;
    exec::agg::QueueStage* _queueStage = nullptr;

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
