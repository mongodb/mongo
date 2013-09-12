/**
 *    Copyright (C) 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

/**
 * Client utilities for upgrade processes, useful operations for safe configuration upgrades.
 *
 * These are not very general purpose, however, so are not in the general cluster utilities
 * libraries.
 */

#pragma once

#include "mongo/client/dbclientinterface.h"

namespace mongo {

    class VersionType;

    /**
     * Verifies that two collections contain documents with the same _ids.
     *
     * @return OK if they do, RemoteValidationError if they do not, and an error Status if
     * anything else went wrong.
     */
    Status checkIdsTheSame(const ConnectionString& configLoc,
                           const std::string& nsA,
                           const std::string& nsB);

    /**
     * Verifies that two collections hash to the same values.
     *
     * @return OK if they do, RemoteValidationError if they do not, and an error Status if
     * anything else went wrong.
     */
    Status checkHashesTheSame(const ConnectionString& configLoc,
                              const std::string& nsA,
                              const std::string& nsB);

    /**
     * Copies a collection (which must not change during this call) to another namespace.  All
     * indexes will also be copied and constructed prior to the data being loaded.
     *
     * @return OK if copy was successful, RemoteValidationError if documents changed during the
     * copy and an error Status if anything else went wrong.
     */
    Status copyFrozenCollection(const ConnectionString& configLoc,
                                const std::string& fromNS,
                                const std::string& toNS);

    /**
     * Atomically overwrites a collection with another collection (only atomic if configLoc is a
     * single server).
     *
     * @return OK if overwrite was successful, and an error Status if anything else went wrong.
     */
    Status overwriteCollection(const ConnectionString& configLoc,
                               const std::string& fromNS,
                               const std::string& overwriteNS);

    /**
     * Creates a suffix for an upgrade's working collection
     */
    string genWorkingSuffix(const OID& lastUpgradeId);

    /**
     * Creates a suffix for an upgrade's backup collection
     */
    string genBackupSuffix(const OID& lastUpgradeId);

    /**
     * Checks whether an unsuccessful upgrade was performed last time and also checks whether
     * the mongos in the current cluster have the mimimum version required. Returns not ok if
     * the check failed and the upgrade should not proceed.
     *
     * Note: There is also a special case for ManualInterventionRequired error where the
     * message will be empty.
     */
    Status preUpgradeCheck(const ConnectionString& configServer,
                           const VersionType& lastVersionInfo,
                           std::string minMongosVersion);

}
