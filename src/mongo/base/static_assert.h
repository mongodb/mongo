// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#define MONGO_STATIC_ASSERT(...) static_assert(__VA_ARGS__, #__VA_ARGS__)

#define MONGO_STATIC_ASSERT_MSG(...) static_assert(__VA_ARGS__)
