/**
 *    Copyright (C) 2013 10gen Inc.
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

/**
 * This module describes free functions for logging various operations of interest to a
 * party interested in generating logs of user activity in a MongoDB server instance.
 */

#pragma once

#include "mongo/base/error_codes.h"

namespace mongo {

    class BSONObj;
    class ClientBasic;
    class NamespaceString;
    class StringData;
    class UserName;

namespace mutablebson {
    class Document;
}  // namespace mutablebson

namespace audit {

    /**
     * Logs the result of an authentication attempt.
     */
    void logAuthentication(ClientBasic* client,
                           const StringData& mechanism,
                           const UserName& user,
                           ErrorCodes::Error result);

    //
    // Authorization (authz) logging functions.
    //
    // These functions generate log messages describing the disposition of access control
    // checks.
    //

    /**
     * Logs the result of a command authorization check.
     */
    void logCommandAuthzCheck(
            ClientBasic* client,
            const NamespaceString& ns,
            const mutablebson::Document& cmdObj,
            ErrorCodes::Error result);

    /**
     * Logs the result of an authorization check for an OP_DELETE wire protocol message.
     */
    void logDeleteAuthzCheck(
            ClientBasic* client,
            const NamespaceString& ns,
            const BSONObj& pattern,
            ErrorCodes::Error result);

    /**
     * Logs the result of an authorization check for the "unlock" pseudo-command.
     */
    void logFsyncUnlockAuthzCheck(
            ClientBasic* client,
            ErrorCodes::Error result);

    /**
     * Logs the result of an authorization check for an OP_GET_MORE wire protocol message.
     */
    void logGetMoreAuthzCheck(
            ClientBasic* client,
            const NamespaceString& ns,
            long long cursorId,
            ErrorCodes::Error result);

    /**
     * Logs the result of an authorization check for an "inprog" pseudo-command.
     */
    void logInProgAuthzCheck(
            ClientBasic* client,
            const BSONObj& filter,
            ErrorCodes::Error result);

    /**
     * Logs the result of an authorization check for an OP_INSERT wire protocol message.
     */
    void logInsertAuthzCheck(
            ClientBasic* client,
            const NamespaceString& ns,
            const BSONObj& insertedObj,
            ErrorCodes::Error result);

    /**
     * Logs the result of an authorization check for an OP_KILL_CURSORS wire protocol message.
     */
    void logKillCursorsAuthzCheck(
            ClientBasic* client,
            const NamespaceString& ns,
            long long cursorId,
            ErrorCodes::Error result);

    /**
     * Logs the result of an authorization check for a "killop" pseudo-command.
     */
    void logKillOpAuthzCheck(
            ClientBasic* client,
            const BSONObj& filter,
            ErrorCodes::Error result);

    /**
     * Logs the result of an authorization check for an OP_QUERY wire protocol message.
     */
    void logQueryAuthzCheck(
            ClientBasic* client,
            const NamespaceString& ns,
            const BSONObj& query,
            ErrorCodes::Error result);

    /**
     * Logs the result of an authorization check for an OP_UPDATE wire protocol message.
     */
    void logUpdateAuthzCheck(
            ClientBasic* client,
            const NamespaceString& ns,
            const BSONObj& query,
            const BSONObj& updateObj,
            bool isUpsert,
            bool isMulti,
            ErrorCodes::Error result);

}  // namespace audit
}  // namespace mongo
