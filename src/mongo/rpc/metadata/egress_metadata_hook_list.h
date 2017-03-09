/**
 *    Copyright (C) 2017 MongoDB Inc.
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
#include <vector>

#include "mongo/rpc/metadata/metadata_hook.h"

namespace mongo {

class BSONObj;
class BSONObjBuilder;
struct HostAndPort;
class OperationContext;
class Status;

namespace rpc {

/**
 * Data structure for storing a list of EgressMetadataHook.
 */
class EgressMetadataHookList final : public EgressMetadataHook {
public:
    /**
     * Adds a hook to this list. The hooks are executed in the order they were added.
     */
    void addHook(std::unique_ptr<EgressMetadataHook>&& newHook);

    /**
     * Calls writeRequestMetadata on every hook in the order they were added. This will terminate
     * early if one of hooks returned a non OK status and return it. Note that metadataBob should
     * not be used if Status is not OK as the contents can be partial.
     */
    Status writeRequestMetadata(OperationContext* opCtx, BSONObjBuilder* metadataBob) override;

    /**
     * Calls readReplyMetadata on every hook in the order they were added. This will terminate
     * early if one of hooks returned a non OK status and return it. Note that metadataBob should
     * not be used if Status is not OK as the contents can be partial.
     */
    Status readReplyMetadata(StringData replySource, const BSONObj& metadataObj) override;

private:
    std::vector<std::unique_ptr<EgressMetadataHook>> _hooks;
};

}  // namespace rpc
}  // namespace mongo
