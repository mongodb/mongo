/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "util.h"

static int list_init_block(WT_SESSION *, const char *, WT_BLOCK *);
static int list_print(WT_SESSION *, const char *, bool, bool);
static int list_print_checkpoint(WT_SESSION *, const char *);

/*
 * usage --
 *     TODO: Add a comment describing this function.
 */
static int
usage(void)
{
    static const char *options[] = {"-c",
      "display checkpoints in human-readable format (by default checkpoints are not displayed)",
      "-v", "display the complete schema table (by default only a subset is displayed)", NULL,
      NULL};

    util_usage("list [-cv] [uri]", "options:", options);
    return (1);
}

/*
 * util_list --
 *     TODO: Add a comment describing this function.
 */
int
util_list(WT_SESSION *session, int argc, char *argv[])
{
    WT_DECL_RET;
    int ch;
    char *uri;
    bool cflag, vflag;

    cflag = vflag = false;
    uri = NULL;
    while ((ch = __wt_getopt(progname, argc, argv, "cv")) != EOF)
        switch (ch) {
        case 'c':
            cflag = true;
            break;
        case 'v':
            vflag = true;
            break;
        case '?':
        default:
            return (usage());
        }
    argc -= __wt_optind;
    argv += __wt_optind;

    switch (argc) {
    case 0:
        break;
    case 1:
        if ((uri = util_uri(session, *argv, "table")) == NULL)
            return (1);
        break;
    default:
        return (usage());
    }

    ret = list_print(session, uri, cflag, vflag);

    free(uri);
    return (ret);
}

/*
 * list_init_block --
 *     Initialize a dummy block structure for a file.
 */
static int
list_init_block(WT_SESSION *session, const char *key, WT_BLOCK *block)
{
    WT_CONFIG_ITEM cval;
    WT_CONFIG_PARSER *parser;
    WT_DECL_RET;
    WT_EXTENSION_API *wt_api;
    int tret;
    char *config;

    WT_CLEAR(*block);

    parser = NULL;
    config = NULL;

    wt_api = session->connection->get_extension_api(session->connection);
    if ((ret = wt_api->metadata_search(wt_api, session, key, &config)) != 0)
        WT_ERR(util_err(session, ret, "%s: WT_EXTENSION_API.metadata_search", key));
    /*
     * The config variable should be set and not NULL, but Coverity is convinced otherwise. This is
     * an infrequent code path. Just add this extra conditional to make it happy.
     */
    if (config == NULL)
        goto err;
    if ((ret = wt_api->config_parser_open(wt_api, session, config, strlen(config), &parser)) != 0)
        WT_ERR(util_err(session, ret, "WT_EXTENSION_API.config_parser_open"));
    if ((ret = parser->get(parser, "allocation_size", &cval)) == 0)
        block->allocsize = (uint32_t)cval.val;
    else if (ret != WT_NOTFOUND)
        WT_ERR(util_err(session, ret, "WT_CONFIG_PARSER.get"));

err:
    if (parser != NULL && (tret = parser->close(parser)) != 0) {
        tret = util_err(session, tret, "WT_CONFIG_PARSER.close");
        if (ret == 0)
            ret = tret;
    }

    free(config);
    return (ret);
}

/*
 * list_print --
 *     List the high-level objects in the database.
 */
static int
list_print(WT_SESSION *session, const char *uri, bool cflag, bool vflag)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *key, *value;
    bool found;

    /* Open the metadata file. */
    if ((ret = session->open_cursor(session, WT_METADATA_URI, NULL, NULL, &cursor)) != 0) {
        /*
         * If there is no metadata (yet), this will return ENOENT. Treat that the same as an empty
         * metadata.
         */
        if (ret == ENOENT)
            return (0);

        return (util_err(session, ret, "%s: WT_SESSION.open_cursor", WT_METADATA_URI));
    }

    found = uri == NULL;
    while ((ret = cursor->next(cursor)) == 0) {
        /* Get the key. */
        if ((ret = cursor->get_key(cursor, &key)) != 0)
            return (util_cerr(cursor, "get_key", ret));

        /* If a name is specified, only show objects that match. */
        if (uri != NULL) {
            if (!WT_PREFIX_MATCH(key, uri))
                continue;
            found = true;
        }

        /*
         * !!!
         * Don't report anything about the WiredTiger metadata and history store since they are not
         * user created objects unless the verbose or checkpoint options are passed in. However,
         * skip over the metadata system information for anything except the verbose option.
         */
        if (!vflag && WT_PREFIX_MATCH(key, WT_SYSTEM_PREFIX))
            continue;
        if (cflag || vflag || (strcmp(key, WT_METADATA_URI) != 0 && strcmp(key, WT_HS_URI) != 0))
            printf("%s\n", key);

        if (!cflag && !vflag)
            continue;

        if (cflag && (ret = list_print_checkpoint(session, key)) != 0)
            return (ret);
        if (vflag) {
            if ((ret = cursor->get_value(cursor, &value)) != 0)
                return (util_cerr(cursor, "get_value", ret));
            printf("%s\n", value);
        }
    }
    if (ret != WT_NOTFOUND)
        return (util_cerr(cursor, "next", ret));
    if (!found) {
        fprintf(stderr, "%s: %s: not found\n", progname, uri);
        return (1);
    }

    return (0);
}

/*
 * list_print_size --
 *     List a size found in the checkpoint information.
 */
static void
list_print_size(uint64_t v)
{
    if (v >= WT_PETABYTE)
        printf("%" PRIu64 " PB", v / WT_PETABYTE);
    else if (v >= WT_TERABYTE)
        printf("%" PRIu64 " TB", v / WT_TERABYTE);
    else if (v >= WT_GIGABYTE)
        printf("%" PRIu64 " GB", v / WT_GIGABYTE);
    else if (v >= WT_MEGABYTE)
        printf("%" PRIu64 " MB", v / WT_MEGABYTE);
    else if (v >= WT_KILOBYTE)
        printf("%" PRIu64 " KB", v / WT_KILOBYTE);
    else
        printf("%" PRIu64 " B", v);
}

/*
 * list_print_checkpoint --
 *     List the checkpoint information.
 */
static int
list_print_checkpoint(WT_SESSION *session, const char *key)
{
    WT_BLOCK _block, *block;
    WT_BLOCK_CKPT ci;
    WT_CKPT *ckpt, *ckptbase;
    WT_DECL_RET;
    size_t len;
    time_t t;

    /*
     * We may not find any checkpoints for this file, in which case we don't report an error, and
     * continue our caller's loop. Otherwise, read the list of checkpoints and print each
     * checkpoint's name and time.
     */
    if ((ret = __wt_metadata_get_ckptlist(session, key, &ckptbase)) != 0)
        return (ret == WT_NOTFOUND ? 0 : ret);

    /* We need the allocation size for decoding the checkpoint addr */
    /* TODO this is a kludge: fix */
    block = &_block;
    if ((ret = list_init_block(session, key, block)) != 0)
        return (ret);

    /* Find the longest name, so we can pretty-print. */
    len = 0;
    WT_CKPT_FOREACH (ckptbase, ckpt)
        if (strlen(ckpt->name) > len)
            len = strlen(ckpt->name);
    ++len;

    memset(&ci, 0, sizeof(ci));
    WT_CKPT_FOREACH (ckptbase, ckpt) {
        /*
         * Call ctime, not ctime_r; ctime_r has portability problems, the Solaris version is
         * different from the POSIX standard.
         */
        if (ckpt != ckptbase)
            printf("\n");
        t = (time_t)ckpt->sec;
        printf("\t%*s: %.24s", (int)len, ckpt->name, ctime(&t));

        printf(" (size ");
        list_print_size(ckpt->size);
        printf(")\n");

        /* Decode the checkpoint block. */
        if (ckpt->raw.data == NULL)
            continue;
        if ((ret = __wt_block_ckpt_decode(session, block, ckpt->raw.data, ckpt->raw.size, &ci)) ==
          0) {
            printf(
              "\t\t"
              "file-size: ");
            list_print_size((uint64_t)ci.file_size);
            printf(", checkpoint-size: ");
            list_print_size(ci.ckpt_size);
            printf("\n\n");

            printf(
              "\t\t"
              "          offset, size, checksum\n");
            printf(
              "\t\t"
              "root    "
              ": %" PRIuMAX ", %" PRIu32 ", %" PRIu32 " (%#" PRIx32 ")\n",
              (uintmax_t)ci.root_offset, ci.root_size, ci.root_checksum, ci.root_checksum);
            printf(
              "\t\t"
              "alloc   "
              ": %" PRIuMAX ", %" PRIu32 ", %" PRIu32 " (%#" PRIx32 ")\n",
              (uintmax_t)ci.alloc.offset, ci.alloc.size, ci.alloc.checksum, ci.alloc.checksum);
            printf(
              "\t\t"
              "discard "
              ": %" PRIuMAX ", %" PRIu32 ", %" PRIu32 " (%#" PRIx32 ")\n",
              (uintmax_t)ci.discard.offset, ci.discard.size, ci.discard.checksum,
              ci.discard.checksum);
            printf(
              "\t\t"
              "avail   "
              ": %" PRIuMAX ", %" PRIu32 ", %" PRIu32 " (%#" PRIx32 ")\n",
              (uintmax_t)ci.avail.offset, ci.avail.size, ci.avail.checksum, ci.avail.checksum);
        } else {
            /* Ignore the error and continue if damaged. */
            (void)util_err(session, ret, "__wt_block_ckpt_decode");
        }
    }

    __wt_metadata_free_ckptlist(session, ckptbase);
    return (0);
}
