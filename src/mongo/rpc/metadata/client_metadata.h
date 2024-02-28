/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <boost/optional/optional.hpp>
#include <string>
#include <utility>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/query/util/deferred.h"

namespace mongo {

class Client;
class OperationContext;

constexpr auto kMetadataDocumentName = "client"_sd;

/**
 * The ClientMetadata class is responsible for parsing the client metadata document that is received
 * in the "client" field of the first hello from clients. The client metadata document can also be
 * parsed from the "$client" field of any operation. This class also provides static methods for
 * client libraries to write a valid client metadata document.
 *
 * Example client metadata document:
 * {
 *     "application" : {              // Optional
 *         "name" : "string"          // Optional with caveats
 *     },
 *     "driver" : {                   // Required, Informational Only
 *         "name" : "string",         // Required, Informational Only
 *         "version" : "string"       // Required, Informational Only
 *     },
 *     "os" : {                       // Required, Informational Only
 *         "type" : "string",         // Required, Informational Only, See note
 *         "name" : "string",         // Optional, Informational Only
 *         "architecture" : "string", // Optional, Informational Only
 *         "version" : "string"       // Optional, Informational Only
 *     }
 *     "mongos" : {                   // Optional, Informational Only
 *         "host" : "string",         // Optional, Informational Only
 *         "client" : "string",       // Optional, Informational Only
 *         "version" : "string"       // Optional, Informational Only
 *     }
 * }
 *
 * It is allowed to contain additional fields that are not listed in the example above. These
 * additional fields are ignore by this class. The "os" document "type" field is required (defaults
 * to "unknown" in Mongo Drivers). The "driver", and "os" documents while required, are for
 * informational purposes only. The content is logged to disk but otherwise ignored.
 *
 * See Driver Specification: "MongoDB Handshake" for more information.
 */
class ClientMetadata {
public:
    explicit ClientMetadata(BSONObj obj);

    ClientMetadata(const ClientMetadata& src) : ClientMetadata(src._document) {}
    ClientMetadata& operator=(const ClientMetadata& src) {
        ClientMetadata copy(src._document);
        *this = std::move(copy);
        return *this;
    }

    ClientMetadata(ClientMetadata&&) = default;
    ClientMetadata& operator=(ClientMetadata&&) = default;

    /**
     * Get the ClientMetadata for the Client.
     *
     * This function may return nullptr if there was no ClientMetadata provided for the
     * Client.
     *
     * The pointer to ClientMetadata is valid to use if:
     * - You hold the Client lock.
     * - You are on the Client's thread.
     */
    static const ClientMetadata* getForClient(Client* client) noexcept;

    /**
     * Get the ClientMetadata for the OperationContext.
     *
     * This function may return nullptr if there was no ClientMetadata provided for the
     * OperationContext.
     *
     * The pointer to ClientMetadata is valid to use if:
     * - You hold the Client lock.
     * - You are on the Client's thread.
     */
    static const ClientMetadata* getForOperation(OperationContext* opCtx) noexcept;

    /**
     * Get the prioritized ClientMetadata for the Client.
     *
     * This function returns getForOperation() if it returns a valid pointer, otherwise it returns
     * getForClient().
     *
     * The pointer to ClientMetadata is valid to use if:
     * - You hold the Client lock.
     * - You are on the Client's thread.
     */
    static const ClientMetadata* get(Client* client) noexcept;

    /**
     * Set the ClientMetadata for the Client directly.
     *
     * This should only be used in testing. It sets the ClientMetadata as finalized but does not
     * check if it was previously finalized. It allows the user to replace the ClientMetadata for
     * a Client, which is disallowed if done via setFromMetadata().
     *
     * This function takes the Client lock.
     */
    static void setAndFinalize(Client* client, boost::optional<ClientMetadata> meta);

    /**
     * Parse and validate a client metadata document contained in a hello request.
     *
     * Empty or non-existent sub-documents are permitted. Non-empty documents are required to have
     * the fields driver.name, driver.version, and os.type which must be strings.
     *
     * Returns an empty optional if element is empty.
     */
    static StatusWith<boost::optional<ClientMetadata>> parse(const BSONElement& element);

    /**
     * Wrapper for BSONObj constructor used by IDL parsers.
     */
    static ClientMetadata parseFromBSON(BSONObj obj) {
        return ClientMetadata(obj);
    }

    /**
     * Create a new client metadata document with os information from the ProcessInfo class.
     *
     * This method outputs the "client" field, and client metadata sub-document in the
     * BSONObjBuilder:
     *
     * "client" : {
     *     "driver" : {
     *         "name" : "string",
     *         "version" : "string"
     *     },
     *     "os" : {
     *         "type" : "string",
     *         "name" : "string",
     *         "architecture" : "string",
     *         "version" : "string"
     *     }
     * }
     */
    static void serialize(StringData driverName, StringData driverVersion, BSONObjBuilder* builder);

    /**
     * Create a new client metadata document with os information from the ProcessInfo class.
     *
     * driverName - name of the driver, must not be empty
     * driverVersion - a string for the driver version, must not be empty
     *
     * Notes: appName must be <= 128 bytes otherwise an error is returned. It may be empty in which
     * case it is omitted from the output document.
     *
     * This method outputs the "client" field, and client metadata sub-document in the
     * BSONObjBuilder:
     *
     * "client" : {
     *     "application" : {
     *         "name" : "string"
     *     },
     *     "driver" : {
     *         "name" : "string",
     *         "version" : "string"
     *     },
     *     "os" : {
     *         "type" : "string",
     *         "name" : "string",
     *         "architecture" : "string",
     *         "version" : "string"
     *     }
     * }
     */
    static Status serialize(StringData driverName,
                            StringData driverVersion,
                            StringData appName,
                            BSONObjBuilder* builder);

    /**
     * Mark the ClientMetadata as finalized.
     *
     * Once this function is called, no future hello can mutate the ClientMetadata.
     *
     * This function is only valid to invoke if you are on the Client's thread. This function takes
     * the Client lock.
     */
    static bool tryFinalize(Client* client);

    /**
     * Set the ClientMetadata for the Client by reading it from the given BSONElement.
     *
     * This function throws if the ClientMetadata has already been finalized but the BSONElement is
     * an object. ClientMetadata is allowed to be set via the first hello only.
     *
     * This function is only valid to invoke if you are on the Client's thread. This function takes
     * the Client lock.
     */
    static void setFromMetadata(Client* client, BSONElement& elem, bool isInternalClient);

    /**
     * Set the ClientMetadata for the OperationContext by reading it from the given BSONElement.
     *
     * This function throws if called more than once for the same OperationContext.
     *
     * This function is only valid to invoke if you are on the Client's thread. This function takes
     * the Client lock.
     */
    static void setFromMetadataForOperation(OperationContext* opCtx, const BSONElement& elem);

    /**
     * Read from the $client field in requests.
     *
     * Throws an error if the $client section is not valid. It is valid for it to not exist though.
     */
    static boost::optional<ClientMetadata> readFromMetadata(const BSONElement& elem);

    /**
     * Write the $client section to request bodies if there is a non-empty client metadata
     * connection with the current client.
     */
    void writeToMetadata(BSONObjBuilder* builder) const noexcept;

    /**
     * Modify the existing client metadata document to include a mongos section.
     *
     * hostAndPort is "host:port" of the running MongoS.
     * monogsClient is "host:port" of the connected driver.
     * version is the version string of MongoS.
     *
     * "mongos" : {
     *     "host" : "string",
     *     "client" : "string",
     *     "version" : "string"
     * }
     */
    void setMongoSMetadata(StringData hostAndPort, StringData mongosClient, StringData version);

    /**
     * Get the Application Name for the client metadata document.
     *
     * Used to log Application Name in slow operation reports, and into system.profile.
     * Return: May be empty.
     */
    StringData getApplicationName() const;

    /**
     * Get the BSON Document of the client metadata document. In the example above in the class
     * comment, this is the document in the "client" field.
     *
     * Return: May be empty.
     */
    const BSONObj& getDocument() const;

    /**
     * A lazily computed (and subsequently cached) copy of the metadata with the mongos info
     * removed. This is useful for collecting query stats where we want to scrub out this
     * high-cardinality field, and we don't want to re-do this computation over and over again.
     */
    const BSONObj& documentWithoutMongosInfo() const;

    /**
     * Get the simple hash of the client metadata document (simple meaning no collation).
     *
     * The hash is generated on the first call to this method. Future calls will return the cached
     * hash rather than recomputing.
     */
    unsigned long hashWithoutMongosInfo() const;

    /**
     * Log client and client metadata information to disk.
     */
    void logClientMetadata(Client* client) const;

    /**
     * Field name for requests that contains client metadata.
     */
    static StringData fieldName();

public:
    /**
     * Create a new client metadata document.
     *
     * driverName - name of the driver
     * driverVersion - a string for the driver version
     * osType - name of host operating system of client, i.e. uname -s
     * osName - name of operating system distro, i.e. "Ubuntu..." or "Microsoft Windows 8"
     * osArchitecture - architecture of host operating system, i.e. uname -p
     * osVersion - operating system version, i.e. uname -v
     *
     * Notes: appName must be <= 128 bytes otherwise an error is returned. It may be empty in which
     * case it is omitted from the output document. All other fields must not be empty.
     *
     * Exposed for Unit Test purposes
     */
    static Status serializePrivate(StringData driverName,
                                   StringData driverVersion,
                                   StringData osType,
                                   StringData osName,
                                   StringData osArchitecture,
                                   StringData osVersion,
                                   StringData appName,
                                   BSONObjBuilder* builder);

private:
    ClientMetadata() = default;

    static Status validateDriverDocument(const BSONObj& doc);
    static Status validateOperatingSystemDocument(const BSONObj& doc);
    static StatusWith<std::string> parseApplicationDocument(const BSONObj& doc);

private:
    // Parsed Client Metadata document
    // May be empty
    // Owned
    BSONObj _document;

    // Application Name extracted from the client metadata document.
    // May be empty
    std::string _appName;

    // See documentWithoutMongosInfo().
    Deferred<BSONObj (*)(const BSONObj&)> _documentWithoutMongosInfo{
        [](const BSONObj& fullDocument) {
            return fullDocument.removeField("mongos");
        }};

    // See hashWithoutMongosInfo().
    Deferred<size_t (*)(const BSONObj&)> _hashWithoutMongos{simpleHash};
};

}  // namespace mongo
