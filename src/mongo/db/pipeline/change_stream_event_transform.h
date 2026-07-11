// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <set>
#include <string>
#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
/**
 * An abstract class responsible for building a change stream event from an oplog entry.
 */
class ChangeStreamEventTransformation {
public:
    using SupportedEvents = StringDataSet;

    ChangeStreamEventTransformation(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                    const DocumentSourceChangeStreamSpec& spec);

    virtual ~ChangeStreamEventTransformation() = default;

    /**
     * Returns the change stream event build from an oplog entry.
     */
    virtual Document applyTransformation(const Document& fromDoc) const = 0;

    /**
     * Returns all the fields names that could be potentially access by the event builder.
     */
    virtual std::set<std::string> getFieldNameDependencies() const = 0;

protected:
    // Construct a resume token for the specified event.
    ResumeTokenData makeResumeToken(Value tsVal,
                                    Value txnOpIndexVal,
                                    Value uuidVal,
                                    std::string_view operationType,
                                    Value documentKey,
                                    Value opDescription) const;

    const DocumentSourceChangeStreamSpec _changeStreamSpec;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
    ResumeTokenData _resumeToken;
};

/*
 * The event builder class to be used for oplog entries with no special behavior.
 */
class ChangeStreamDefaultEventTransformation final : public ChangeStreamEventTransformation {
    struct SupportedEventResult {
        std::string_view opType;
        Value opDescription;
        bool isBuiltInEvent;
    };

public:
    ChangeStreamDefaultEventTransformation(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           const DocumentSourceChangeStreamSpec& spec);

    Document applyTransformation(const Document& fromDoc) const override;
    std::set<std::string> getFieldNameDependencies() const override;

    const ChangeStreamEventTransformation::SupportedEvents& getSupportedEvents_forTest() const {
        return _supportedEvents;
    }

private:
    /**
     * Checks the 'o2Field' value of an oplog entry has any field name that is contained in
     * '_supportedEvents'. If so, it returns the name of the field, the value mapped to the field
     * in the oplog entry, as well as whether the event is a builtin event. Otherwise returns
     * 'boost::none'.
     */
    boost::optional<SupportedEventResult> handleSupportedEvent(const Document& o2Field) const;

    /**
     * Build the '_supportedEvents' container from the 'supportedEvents' change stream parameter.
     * Can throw exceptions if 'supportedEvents' contains invalid values.
     */
    ChangeStreamEventTransformation::SupportedEvents buildSupportedEvents() const;

    /**
     * Additional supported events that this transformer can handle. These events can be created off
     * of "noop" oplog entries which have any of the supported events as a field name inside their
     * 'o2' field value.
     */
    ChangeStreamEventTransformation::SupportedEvents _supportedEvents;

    /**
     * Set to true if the pre-image should be included in the output documents.
     */
    const bool _preImageRequested = false;

    /**
     * Set to true if the post-image should be included in the output documents.
     */
    const bool _postImageRequested = false;

    /**
     * If set to 'true', the change stream will emit a 'fromMigrate' field with a value of 'true'
     * for all events originating from a migration. This requires the change stream to be opened
     * with the 'showMigrationEvents' flag and also the 'changeStreamsEmitFromMigrate' server
     * parameter to be enabled.
     */
    const bool _emitFromMigrateField = false;
};

/**
 * The event builder class to be used for oplog entries with the 'system.views' namespace.
 * It only handles CRUD oplog entries ('insert', 'update', 'delete').
 */
class ChangeStreamViewDefinitionEventTransformation final : public ChangeStreamEventTransformation {
public:
    ChangeStreamViewDefinitionEventTransformation(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentSourceChangeStreamSpec& spec);

    Document applyTransformation(const Document& fromDoc) const override;

    std::set<std::string> getFieldNameDependencies() const override;
};

/**
 * Responsible for deciding which 'ChangeStreamEventTransformation' implementation to use for a
 * given oplog entry. Also holds the ownership of all the 'ChangeStreamEventTransformation'
 * implementations.
 */
class ChangeStreamEventTransformer {
public:
    ChangeStreamEventTransformer(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 const DocumentSourceChangeStreamSpec& spec);

    Document applyTransformation(const Document& oplog) const {
        return getBuilder(oplog)->applyTransformation(oplog);
    }

    std::set<std::string> getFieldNameDependencies() const {
        auto accessedFields = _defaultEventBuilder->getFieldNameDependencies();
        accessedFields.merge(_viewNsEventBuilder->getFieldNameDependencies());
        return accessedFields;
    }

    // TODO SERVER-112709: Split change stream document source tests into QO and QE parts
    [[MONGO_MOD_NEEDS_REPLACEMENT]]
    const ChangeStreamEventTransformation::SupportedEvents& getSupportedEvents_forTest() const {
        return _defaultEventBuilder->getSupportedEvents_forTest();
    }

private:
    ChangeStreamEventTransformation* getBuilder(const Document& oplog) const;

    std::unique_ptr<ChangeStreamDefaultEventTransformation> _defaultEventBuilder;
    std::unique_ptr<ChangeStreamViewDefinitionEventTransformation> _viewNsEventBuilder;

    const bool _isSingleCollStream;
};

}  // namespace mongo
