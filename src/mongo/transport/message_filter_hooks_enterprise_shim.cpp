/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 */

// On enterprise Linux builds, libmessage_filter_hooks.so is a delegation wrapper that re-exports
// libmessage_filter_hooks_enterprise.so. This file exists solely to ensure the wrapper is compiled
// with at least one translation unit, which is required by build tools such as Coverity that do not
// support source-less shared libraries. The real implementations are provided by
// message_filter_loader.cpp in the enterprise module.
