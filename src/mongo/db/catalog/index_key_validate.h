/**
*    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/server_options.h"

namespace mongo {
class BSONObj;
class CollatorInterface;
class NamespaceString;
class Status;
template <typename T>
class StatusWith;

namespace index_key_validate {

/**
 * Checks if the key is valid for building an index according to the validation rules for the given
 * index version.
 */
Status validateKeyPattern(const BSONObj& key, IndexDescriptor::IndexVersion indexVersion);

/**
 * Validates the index specification 'indexSpec' and returns an equivalent index specification that
 * has any missing attributes filled in. If the index specification is malformed, then an error
 * status is returned.
 */
StatusWith<BSONObj> validateIndexSpec(
    OperationContext* opCtx,
    const BSONObj& indexSpec,
    const NamespaceString& expectedNamespace,
    const ServerGlobalParams::FeatureCompatibility& featureCompatibility);

/**
 * Performs additional validation for _id index specifications. This should be called after
 * validateIndexSpec().
 */
Status validateIdIndexSpec(const BSONObj& indexSpec);

/**
 * Confirms that 'indexSpec' contains only valid field names. Returns an error if an unexpected
 * field name is found.
 */
Status validateIndexSpecFieldNames(const BSONObj& indexSpec);

/**
 * Validates the 'collation' field in the index specification 'indexSpec' and fills in the full
 * collation spec. If 'collation' is missing, fills it in with the spec for 'defaultCollator'.
 * Returns the index specification with 'collation' filled in.
 */
StatusWith<BSONObj> validateIndexSpecCollation(OperationContext* opCtx,
                                               const BSONObj& indexSpec,
                                               const CollatorInterface* defaultCollator);

}  // namespace index_key_validate
}  // namespace mongo
