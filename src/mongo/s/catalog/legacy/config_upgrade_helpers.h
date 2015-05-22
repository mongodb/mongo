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

#include <string>

namespace mongo {

    class CatalogManager;
    class ConnectionString;
    class OID;
    class Status;
    class VersionType;

    /**
     * Checks whether an unsuccessful upgrade was performed last time and also checks whether
     * the mongos in the current cluster have the mimimum version required. Returns not ok if
     * the check failed and the upgrade should not proceed.
     *
     * Note: There is also a special case for ManualInterventionRequired error where the
     * message will be empty.
     */
    Status preUpgradeCheck(CatalogManager* catalogManager,
                           const VersionType& lastVersionInfo,
                           std::string minMongosVersion);

    /**
     * Informs the config server that the upgrade task was completed by bumping the version.
     * This also clears all upgrade state effectively leaving the critical section if the
     * upgrade process did enter it.
     */
    Status commitConfigUpgrade(CatalogManager* catalogManager,
                               int currentVersion,
                               int minCompatibleVersion,
                               int newVersion);

} // namespace mongo
