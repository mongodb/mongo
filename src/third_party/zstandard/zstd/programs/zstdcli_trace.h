/*
 * Copyright (c) Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#ifndef ZSTDCLI_TRACE_H
#define ZSTDCLI_TRACE_H

/**
 * Enable tracing - log to filename.
 */
void TRACE_enable(char const* filename);

/**
 * Shut down the tracing library.
 */
void TRACE_finish(void);

#endif /* ZSTDCLI_TRACE_H */
