/*
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

#include "mongo/platform/basic.h"

#include "mongo/rpc/metadata/server_selection_metadata.h"

#include <utility>
#include <tuple>

#include "mongo/base/status_with.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace rpc {

namespace {

    const char kSecondaryOkFieldName[] = "$secondaryOk";
    const char kReadPreferenceFieldName[] = "$readPreference";

    const char kQueryOptionsFieldName[] = "$queryOptions";

    const char kDollarQueryWrapper[] = "$query";
    const char kQueryWrapper[] = "query";

    /**
     * Utility to unwrap a '$query' or 'query' wrapped command object. The first element of the
     * return value indicates whether the command was unwrapped, and the second element is either
     * the unwrapped command (if it was wrapped), or the original command if it was not.
     */
    std::tuple<bool, BSONObj> unwrapCommand(const BSONObj& maybeWrapped) {
        const auto firstElFieldName = maybeWrapped.firstElementFieldName();
        if ((firstElFieldName == StringData(kDollarQueryWrapper)) ||
            (firstElFieldName == StringData(kQueryWrapper))) {
            // TODO: do we need getOwned here?
            return std::make_tuple(true, maybeWrapped.firstElement().embeddedObject());
        }
        return std::make_tuple(false, maybeWrapped);
    }

    /**
     * Reads a top-level $readPreference field from a wrapped command.
     */
    Status extractWrappedReadPreference(const BSONObj& wrappedCommand,
                                        BSONObjBuilder* metadataBob) {
        BSONElement readPrefEl;
        auto rpExtractStatus = bsonExtractTypedField(wrappedCommand,
                                                     kReadPreferenceFieldName,
                                                     mongo::Object,
                                                     &readPrefEl);
        if (rpExtractStatus.isOK()) {
            metadataBob->append(readPrefEl);
        }
        else if (rpExtractStatus != ErrorCodes::NoSuchKey) {
            return rpExtractStatus;
        }

        return Status::OK();
    }

    /**
     * Reads a $readPreference from a $queryOptions subobject, if it exists, and writes it to
     * metadataBob. Writes out the original command excluding the $queryOptions subobject.
     */
    Status extractUnwrappedReadPreference(const BSONObj& unwrappedCommand,
                                          BSONObjBuilder* commandBob,
                                          BSONObjBuilder* metadataBob) {
        BSONElement queryOptionsEl;
        BSONElement readPrefEl;

        auto queryOptionsExtractStatus = bsonExtractTypedField(unwrappedCommand,
                                                               kQueryOptionsFieldName,
                                                               mongo::Object,
                                                               &queryOptionsEl);

        // If there is no queryOptions subobject, we write out the command and return.
        if (queryOptionsExtractStatus == ErrorCodes::NoSuchKey) {
            commandBob->appendElements(unwrappedCommand);
            return Status::OK();
        }
        else if (!queryOptionsExtractStatus.isOK()) {
            return queryOptionsExtractStatus;
        }

        // Write out the command excluding the $queryOptions field.
        for (const auto& elem : unwrappedCommand) {
            if (elem.fieldNameStringData() != kQueryOptionsFieldName) {
                commandBob->append(elem);
            }
        }

        auto rpExtractStatus = bsonExtractTypedField(queryOptionsEl.embeddedObject(),
                                                     kReadPreferenceFieldName,
                                                     mongo::Object,
                                                     &readPrefEl);

        // If there is a $queryOptions field, we expect there to be a $readPreference.
        if (!rpExtractStatus.isOK()) {
            return rpExtractStatus;
        }

        metadataBob->append(readPrefEl);
        return Status::OK();
    }

}  // namespace

    const OperationContext::Decoration<ServerSelectionMetadata> ServerSelectionMetadata::get =
        OperationContext::declareDecoration<ServerSelectionMetadata>();

    ServerSelectionMetadata::ServerSelectionMetadata(
        bool secondaryOk,
        boost::optional<ReadPreferenceSetting> readPreference
    )
        : _secondaryOk(secondaryOk)
        , _readPreference(std::move(readPreference))
    {}

    StatusWith<ServerSelectionMetadata>
    ServerSelectionMetadata::readFromMetadata(const BSONObj& metadata) {
        auto secondaryOkField = metadata.getField(kSecondaryOkFieldName);

        bool secondaryOk = !secondaryOkField.eoo();

        boost::optional<ReadPreferenceSetting> readPreference;
        BSONElement rpElem;
        auto readPrefExtractStatus = bsonExtractTypedField(metadata,
                                                           kReadPreferenceFieldName,
                                                           mongo::Object,
                                                           &rpElem);

        if (readPrefExtractStatus == ErrorCodes::NoSuchKey) {
            // Do nothing, it's valid to have no ReadPreference
        }
        else if (!readPrefExtractStatus.isOK()) {
            return readPrefExtractStatus;
        }
        else {
            // We have a read preference in the metadata object.
            auto parsedRps = ReadPreferenceSetting::fromBSON(rpElem.Obj());
            if (!parsedRps.isOK()) {
                return parsedRps.getStatus();
            }
            readPreference.emplace(std::move(parsedRps.getValue()));
        }

        return ServerSelectionMetadata(secondaryOk, std::move(readPreference));
    }

    Status ServerSelectionMetadata::writeToMetadata(BSONObjBuilder* metadataBob) const {
        if (isSecondaryOk()) {
            metadataBob->append(kSecondaryOkFieldName, 1);
        }

        if (getReadPreference()) {
            metadataBob->append(kReadPreferenceFieldName, getReadPreference()->toBSON());
        }

        return Status::OK();
    }

    Status ServerSelectionMetadata::downconvert(const BSONObj& command,
                                                const BSONObj& metadata,
                                                BSONObjBuilder* legacyCommand,
                                                int* legacyQueryFlags) {

        BSONElement secondaryOkElem = metadata.getField(kSecondaryOkFieldName);
        BSONElement readPrefElem = metadata.getField(kReadPreferenceFieldName);

        if (!secondaryOkElem.eoo()) {
            *legacyQueryFlags |= mongo::QueryOption_SlaveOk;
        }
        else {
            *legacyQueryFlags &= ~mongo::QueryOption_SlaveOk;
        }

        if (!readPrefElem.eoo()) {
            // Use 'query' to wrap query, then append read preference.

            // NOTE(amidvidy): Oddly, the _isSecondaryQuery implementation in dbclient_rs does
            // not unwrap the query properly - it only checks for 'query', and not
            // '$query'. We should probably standardize on one - drivers use '$query',
            // and the shell uses 'query'. See SERVER-18705 for details.

            // TODO: this may need to use the $queryOptions hack on mongos.
            legacyCommand->append(kQueryWrapper, command);
            legacyCommand->append(readPrefElem);
        }
        else {
            legacyCommand->appendElements(command);
        }

        return Status::OK();
    }

    Status ServerSelectionMetadata::upconvert(const BSONObj& legacyCommand,
                                              const int legacyQueryFlags,
                                              BSONObjBuilder* commandBob,
                                              BSONObjBuilder* metadataBob) {

        // The secondaryOK option is equivalent to the slaveOk bit being set on legacy commands.
        if (legacyQueryFlags & QueryOption_SlaveOk) {
            metadataBob->append(kSecondaryOkFieldName, 1);
        }

        // First we need to check if we have a wrapped command. That is, a command of the form
        // {'$query': { 'commandName': 1, ...}, '$someOption': 5, ....}. Curiously, the field name
        // of the wrapped query can be either '$query', or 'query'.
        BSONObj maybeUnwrapped;
        bool wasWrapped;
        std::tie(wasWrapped, maybeUnwrapped) = unwrapCommand(legacyCommand);

        if (wasWrapped) {
            // Check if legacyCommand has an invalid $maxTimeMS option.
            // TODO: Move this check elsewhere when we handle upconverting/downconverting maxTimeMS.
            if (legacyCommand.hasField("$maxTimeMS")) {
                return Status(ErrorCodes::InvalidOptions,
                              "cannot use $maxTimeMS query option with "
                              "commands; use maxTimeMS command option "
                              "instead");
            }

            // If the command was wrapped, we can write out the upconverted command now, as there
            // is nothing else we need to remove from it.
            commandBob->appendElements(maybeUnwrapped);

            return extractWrappedReadPreference(legacyCommand, metadataBob);
        }

        // If the command was not wrapped, we need to check for a readPreference sent by mongos
        // on the $queryOptions field of the command. If it is set, we remove it from the
        // upconverted command, so we need to pass the command builder along.
        return extractUnwrappedReadPreference(maybeUnwrapped, commandBob, metadataBob);
    }

    bool ServerSelectionMetadata::isSecondaryOk() const {
        return _secondaryOk;
    }

    const boost::optional<ReadPreferenceSetting>&
    ServerSelectionMetadata::getReadPreference() const {
        return _readPreference;
    }

#if defined(_MSC_VER) && _MSC_VER < 1900
    ServerSelectionMetadata::ServerSelectionMetadata(ServerSelectionMetadata&& ssm)
        : _secondaryOk(ssm._secondaryOk)
        , _readPreference(std::move(ssm._readPreference))
    {}

    ServerSelectionMetadata& ServerSelectionMetadata::operator=(ServerSelectionMetadata&& ssm) {
        _secondaryOk = ssm._secondaryOk;
        _readPreference = std::move(ssm._readPreference);
        return *this;
    }
#endif

}  // rpc
}  // mongo
