/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {

class Client;
class OperationContext;

constexpr auto kMetadataDocumentName = "client"_sd;

/**
 * The ClientMetadata class is responsible for parsing the client metadata document that is received
 * in isMaster from clients. This class also provides static methods for client libraries to create
 * a valid client metadata document.
 *
 * Example document of isMaster request with client metadata document:
 * {
 *    "isMaster" : 1,
 *    "client" : {
 *        "application" : {              // Optional
 *            "name" : "string"          // Optional with caveats
 *        },
 *        "driver" : {                   // Required, Informational Only
 *            "name" : "string",         // Required, Informational Only
 *            "version" : "string"       // Required, Informational Only
 *        },
 *        "os" : {                       // Required, Informational Only
 *            "type" : "string",         // Required, Informational Only, See note
 *            "name" : "string",         // Optional, Informational Only
 *            "architecture" : "string", // Optional, Informational Only
 *            "version" : "string"       // Optional, Informational Only
 *        }
 *    }
 * }
 *
 * For this classes' purposes, the client metadata document is the sub-document in "client". It is
 * allowed to contain additional fields that are not listed in the example above. These additional
 * fields are ignore by this class. The "os" document "type" field is required (defaults to
 * "unknown" in Mongo Drivers). The "driver", and "os" documents while required, are for
 * informational purposes only. The content is logged to disk but otherwise ignored.
 *
 * See Driver Specification: "MongoDB Handshake" for more information.
 */
class ClientMetadata {
    MONGO_DISALLOW_COPYING(ClientMetadata);

public:
    ClientMetadata(ClientMetadata&&) = default;
    ClientMetadata& operator=(ClientMetadata&&) = default;

    /**
     * Parse and validate a client metadata document contained in an isMaster request.
     *
     * Empty or non-existent sub-documents are permitted. Non-empty documents are required to have
     * the fields driver.name, driver.version, and os.type which must be strings.
     *
     * Returns an empty optional if element is empty.
     */
    static StatusWith<boost::optional<ClientMetadata>> parse(const BSONElement& element);

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
     * Log client and client metadata information to disk.
     */
    void logClientMetadata(Client* client) const;

    /**
     * Field name for OP_Command metadata that contains client metadata.
     */
    static StringData fieldName();

public:
    /**
     * Create a new client metadata document.
     *
     * Exposed for Unit Test purposes
     */
    static void serializePrivate(StringData driverName,
                                 StringData driverVersion,
                                 StringData osType,
                                 StringData osName,
                                 StringData osArchitecture,
                                 StringData osVersion,
                                 BSONObjBuilder* builder);

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

    Status parseClientMetadataDocument(const BSONObj& doc);
    static Status validateDriverDocument(const BSONObj& doc);
    static Status validateOperatingSystemDocument(const BSONObj& doc);
    static StatusWith<StringData> parseApplicationDocument(const BSONObj& doc);

private:
    // Parsed Client Metadata document
    // May be empty
    // Owned
    BSONObj _document;

    // Application Name extracted from the client metadata document.
    // May be empty
    StringData _appName;
};

}  // namespace mongo
