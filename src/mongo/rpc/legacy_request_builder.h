/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/document_range.h"
#include "mongo/rpc/request_builder_interface.h"
#include "mongo/util/net/message.h"

namespace mongo {
namespace rpc {

class LegacyRequestBuilder : public RequestBuilderInterface {
public:
    LegacyRequestBuilder();
    ~LegacyRequestBuilder() final;

    LegacyRequestBuilder(Message&&);

    LegacyRequestBuilder& setDatabase(StringData database) final;
    LegacyRequestBuilder& setCommandName(StringData commandName) final;
    LegacyRequestBuilder& setMetadata(BSONObj metadata) final;
    LegacyRequestBuilder& setCommandArgs(BSONObj commandArgs) final;

    LegacyRequestBuilder& addInputDocs(DocumentRange inputDocs) final;
    LegacyRequestBuilder& addInputDoc(BSONObj inputDoc) final;

    State getState() const final;

    Protocol getProtocol() const final;

    Message done() final;

private:
    Message _message;
    BufBuilder _builder{};

    // we need to stash this as we need metadata to
    // upconvert.
    BSONObj _commandArgs;

    std::string _ns{};  // copied to in setDatabase

    State _state{State::kDatabase};
};

}  // namespace rpc
}  // namespace mongo
