#include "wiredtiger_config.h"

#ifdef HAVE_HYPERLEVELDB
#include <hyperleveldb/cache.h>
#include <hyperleveldb/comparator.h>
#include <hyperleveldb/db.h>
#include <hyperleveldb/env.h>
#include <hyperleveldb/filter_policy.h>
#include <hyperleveldb/slice.h>
#include <hyperleveldb/slice.h>
#include <hyperleveldb/status.h>
#include <hyperleveldb/table_builder.h>
#include <hyperleveldb/write_batch.h>
#else
#include "cache.h"
#include "comparator.h"
#include "db.h"
#include "env.h"
#include "filter_policy.h"
#include "slice.h"
#include "slice.h"
#include "status.h"
#include "table_builder.h"
#include "write_batch.h"
#endif

#include "wiredtiger.h"
