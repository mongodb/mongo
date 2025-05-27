/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/util/string_map.h"

#include <memory>
#include <set>
#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
/**
 * An abstract class responsible for building a change stream event from an oplog entry.
 */
class ChangeStreamEventTransformation {
public:
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
                                    StringData operationType,
                                    Value documentKey,
                                    Value opDescription) const;

    const DocumentSourceChangeStreamSpec _changeStreamSpec;
    boost::intrusive_ptr<ExpressionContext> _expCtx;
    ResumeTokenData _resumeToken;

    // Set to true if the pre-image should be included in the output documents.
    bool _preImageRequested = false;

    // Set to true if the post-image should be included in the output documents.
    bool _postImageRequested = false;
};

/*
 * The event builder class to be used for oplog entries with no special behavior.
 */
class ChangeStreamDefaultEventTransformation final : public ChangeStreamEventTransformation {
public:
    using SupportedEvents = StringDataSet;

    ChangeStreamDefaultEventTransformation(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           const DocumentSourceChangeStreamSpec& spec);

    Document applyTransformation(const Document& fromDoc) const override;
    std::set<std::string> getFieldNameDependencies() const override;

private:
    /**
     * Checks the 'o2Field' value of an oplog entry has any field name that is contained in
     * '_supportedEvents'. If so, it returns the name of the field and the value mapped to the field
     * in the oplog entry. Otherwise returns 'boost::none'.
     */
    boost::optional<std::pair<StringData, Value>> handleSupportedEvent(
        const Document& o2Field) const;

    /**
     * Build the '_supportedEvents' container from the 'supportedEvents' change stream parameter.
     * Can throw exceptions if 'supportedEvents' contains invalid values.
     */
    SupportedEvents buildSupportedEvents() const;

    /**
     * Additional supported events that this transformer can handle. These events can be created off
     * of "noop" oplog entries which have any of the supported events as a field name inside their
     * 'o2' field value.
     */
    SupportedEvents _supportedEvents;
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

private:
    ChangeStreamEventTransformation* getBuilder(const Document& oplog) const;

    std::unique_ptr<ChangeStreamDefaultEventTransformation> _defaultEventBuilder;
    std::unique_ptr<ChangeStreamViewDefinitionEventTransformation> _viewNsEventBuilder;

    bool _isSingleCollStream;
};

}  // namespace mongo
