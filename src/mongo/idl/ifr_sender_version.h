/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/idl/generic_argument_gen.h"
#include "mongo/util/version.h"

namespace mongo {

/**
 * Builds an 'IFRSenderVersion' from a 'VersionInfoInterface', decomposing the running binary's
 * version into its (major, minor, patch, extra) components. The 'extra' component encodes
 * pre-release ordering, so comparing the result against another 'IFRSenderVersion' yields correct
 * version ordering (see the IFRSenderVersion definition in generic_argument.idl).
 */
IFRSenderVersion makeIFRSenderVersion(const VersionInfoInterface& provider);

/**
 * Convenience overload that uses the process-wide 'VersionInfoInterface::instance()'.
 */
IFRSenderVersion makeLocalIFRSenderVersion();

}  // namespace mongo
