/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/extension/sdk/handle.h"

namespace mongo::extension::host {
/**
 * OwnedHandle is a move-only wrapper around a raw pointer allocated by the extension, whose
 * ownership has been transferred to the host. OwnedHandle acts as a wrapper that
 * abstracts the vtable and underlying pointer, and makes sure to destroy the associated pointer
 * when it goes out of scope.
 *
 * Note, for the time being, we are building and linking the C++ SDK into the host API to minimize
 * code duplication. Once we are ready to decouple the C++ SDK from the server, we will need to
 * provide a copy of the implementation of these classes within the host API.
 */
template <typename T>
using OwnedHandle = sdk::OwnedHandle<T>;

/**
 * UnownedHandle is a wrapper around a raw pointer allocated by the extension, whose
 * ownership has not been transferred to the host. UnownedHandle acts as a wrapper that
 * abstracts the vtable and underlying pointer, but does not destroy the pointer when it goes out of
 * scope.
 *
 * Note, for the time being, we are building and linking the C++ SDK into the host API to minimize
 * code duplication. Once we are ready to decouple the C++ SDK from the server, we will need to
 * provide a copy of the implementation of these classes within the host API.
 */
template <typename T>
using UnownedHandle = sdk::UnownedHandle<T>;
}  // namespace mongo::extension::host
