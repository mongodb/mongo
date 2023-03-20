/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2012,2013 Magnus Edenhill
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

// FIXME: Revise this documentation:
/**
 * This file implements the consumer offset storage.
 * It currently supports local file storage and broker OffsetCommit storage.
 *
 * Regardless of commit method (file, broker, ..) this is how it works:
 *  - When rdkafka, or the application, depending on if auto.offset.commit
 *    is enabled or not, calls rd_kafka_offset_store() with an offset to store,
 *    all it does is set rktp->rktp_stored_offset to this value.
 *    This can happen from any thread and is locked by the rktp lock.
 *  - The actual commit/write of the offset to its backing store (filesystem)
 *    is performed by the main rdkafka thread and scheduled at the configured
 *    auto.commit.interval.ms interval.
 *  - The write is performed in the main rdkafka thread (in a blocking manner
 *    for file based offsets) and once the write has
 *    succeeded rktp->rktp_committed_offset is updated to the new value.
 *  - If offset.store.sync.interval.ms is configured the main rdkafka thread
 *    will also make sure to fsync() each offset file accordingly. (file)
 */


#include "rdkafka_int.h"
#include "rdkafka_topic.h"
#include "rdkafka_partition.h"
#include "rdkafka_offset.h"
#include "rdkafka_broker.h"

#include <stdio.h>
#include <sys/types.h>
#include <fcntl.h>

#ifdef _WIN32
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <shlwapi.h>
#endif


/**
 * Convert an absolute or logical offset to string.
 */
const char *rd_kafka_offset2str(int64_t offset) {
        static RD_TLS char ret[16][32];
        static RD_TLS int i = 0;

        i = (i + 1) % 16;

        if (offset >= 0)
                rd_snprintf(ret[i], sizeof(ret[i]), "%" PRId64, offset);
        else if (offset == RD_KAFKA_OFFSET_BEGINNING)
                return "BEGINNING";
        else if (offset == RD_KAFKA_OFFSET_END)
                return "END";
        else if (offset == RD_KAFKA_OFFSET_STORED)
                return "STORED";
        else if (offset == RD_KAFKA_OFFSET_INVALID)
                return "INVALID";
        else if (offset <= RD_KAFKA_OFFSET_TAIL_BASE)
                rd_snprintf(ret[i], sizeof(ret[i]), "TAIL(%lld)",
                            llabs(offset - RD_KAFKA_OFFSET_TAIL_BASE));
        else
                rd_snprintf(ret[i], sizeof(ret[i]), "%" PRId64 "?", offset);

        return ret[i];
}

static void rd_kafka_offset_file_close(rd_kafka_toppar_t *rktp) {
        if (!rktp->rktp_offset_fp)
                return;

        fclose(rktp->rktp_offset_fp);
        rktp->rktp_offset_fp = NULL;
}


#ifndef _WIN32
/**
 * Linux version of open callback providing racefree CLOEXEC.
 */
int rd_kafka_open_cb_linux(const char *pathname,
                           int flags,
                           mode_t mode,
                           void *opaque) {
#ifdef O_CLOEXEC
        return open(pathname, flags | O_CLOEXEC, mode);
#else
        return rd_kafka_open_cb_generic(pathname, flags, mode, opaque);
#endif
}
#endif

/**
 * Fallback version of open_cb NOT providing racefree CLOEXEC,
 * but setting CLOEXEC after file open (if FD_CLOEXEC is defined).
 */
int rd_kafka_open_cb_generic(const char *pathname,
                             int flags,
                             mode_t mode,
                             void *opaque) {
#ifndef _WIN32
        int fd;
        int on = 1;
        fd     = open(pathname, flags, mode);
        if (fd == -1)
                return -1;
#ifdef FD_CLOEXEC
        fcntl(fd, F_SETFD, FD_CLOEXEC, &on);
#endif
        return fd;
#else
        int fd;
        if (_sopen_s(&fd, pathname, flags, _SH_DENYNO, mode) != 0)
                return -1;
        return fd;
#endif
}


static int rd_kafka_offset_file_open(rd_kafka_toppar_t *rktp) {
        rd_kafka_t *rk = rktp->rktp_rkt->rkt_rk;
        int fd;

#ifndef _WIN32
        mode_t mode = 0644;
#else
        mode_t mode = _S_IREAD | _S_IWRITE;
#endif
        if ((fd = rk->rk_conf.open_cb(rktp->rktp_offset_path, O_CREAT | O_RDWR,
                                      mode, rk->rk_conf.opaque)) == -1) {
                rd_kafka_op_err(rktp->rktp_rkt->rkt_rk, RD_KAFKA_RESP_ERR__FS,
                                "%s [%" PRId32
                                "]: "
                                "Failed to open offset file %s: %s",
                                rktp->rktp_rkt->rkt_topic->str,
                                rktp->rktp_partition, rktp->rktp_offset_path,
                                rd_strerror(errno));
                return -1;
        }

        rktp->rktp_offset_fp =
#ifndef _WIN32
            fdopen(fd, "r+");
#else
            _fdopen(fd, "r+");
#endif

        return 0;
}


static int64_t rd_kafka_offset_file_read(rd_kafka_toppar_t *rktp) {
        char buf[22];
        char *end;
        int64_t offset;
        size_t r;

        if (fseek(rktp->rktp_offset_fp, 0, SEEK_SET) == -1) {
                rd_kafka_op_err(rktp->rktp_rkt->rkt_rk, RD_KAFKA_RESP_ERR__FS,
                                "%s [%" PRId32
                                "]: "
                                "Seek (for read) failed on offset file %s: %s",
                                rktp->rktp_rkt->rkt_topic->str,
                                rktp->rktp_partition, rktp->rktp_offset_path,
                                rd_strerror(errno));
                rd_kafka_offset_file_close(rktp);
                return RD_KAFKA_OFFSET_INVALID;
        }

        r = fread(buf, 1, sizeof(buf) - 1, rktp->rktp_offset_fp);
        if (r == 0) {
                rd_kafka_dbg(rktp->rktp_rkt->rkt_rk, TOPIC, "OFFSET",
                             "%s [%" PRId32 "]: offset file (%s) is empty",
                             rktp->rktp_rkt->rkt_topic->str,
                             rktp->rktp_partition, rktp->rktp_offset_path);
                return RD_KAFKA_OFFSET_INVALID;
        }

        buf[r] = '\0';

        offset = strtoull(buf, &end, 10);
        if (buf == end) {
                rd_kafka_op_err(rktp->rktp_rkt->rkt_rk, RD_KAFKA_RESP_ERR__FS,
                                "%s [%" PRId32
                                "]: "
                                "Unable to parse offset in %s",
                                rktp->rktp_rkt->rkt_topic->str,
                                rktp->rktp_partition, rktp->rktp_offset_path);
                return RD_KAFKA_OFFSET_INVALID;
        }


        rd_kafka_dbg(rktp->rktp_rkt->rkt_rk, TOPIC, "OFFSET",
                     "%s [%" PRId32 "]: Read offset %" PRId64
                     " from offset "
                     "file (%s)",
                     rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                     offset, rktp->rktp_offset_path);

        return offset;
}


/**
 * Sync/flush offset file.
 */
static int rd_kafka_offset_file_sync(rd_kafka_toppar_t *rktp) {
        if (!rktp->rktp_offset_fp)
                return 0;

        rd_kafka_dbg(rktp->rktp_rkt->rkt_rk, TOPIC, "SYNC",
                     "%s [%" PRId32 "]: offset file sync",
                     rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition);

#ifndef _WIN32
        (void)fflush(rktp->rktp_offset_fp);
        (void)fsync(fileno(rktp->rktp_offset_fp));  // FIXME
#else
        // FIXME
        // FlushFileBuffers(_get_osfhandle(fileno(rktp->rktp_offset_fp)));
#endif
        return 0;
}


/**
 * Write offset to offset file.
 *
 * Locality: toppar's broker thread
 */
static rd_kafka_resp_err_t
rd_kafka_offset_file_commit(rd_kafka_toppar_t *rktp) {
        rd_kafka_topic_t *rkt = rktp->rktp_rkt;
        int attempt;
        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;
        int64_t offset          = rktp->rktp_stored_offset;

        for (attempt = 0; attempt < 2; attempt++) {
                char buf[22];
                int len;

                if (!rktp->rktp_offset_fp)
                        if (rd_kafka_offset_file_open(rktp) == -1)
                                continue;

                if (fseek(rktp->rktp_offset_fp, 0, SEEK_SET) == -1) {
                        rd_kafka_op_err(
                            rktp->rktp_rkt->rkt_rk, RD_KAFKA_RESP_ERR__FS,
                            "%s [%" PRId32
                            "]: "
                            "Seek failed on offset file %s: %s",
                            rktp->rktp_rkt->rkt_topic->str,
                            rktp->rktp_partition, rktp->rktp_offset_path,
                            rd_strerror(errno));
                        err = RD_KAFKA_RESP_ERR__FS;
                        rd_kafka_offset_file_close(rktp);
                        continue;
                }

                len = rd_snprintf(buf, sizeof(buf), "%" PRId64 "\n", offset);

                if (fwrite(buf, 1, len, rktp->rktp_offset_fp) < 1) {
                        rd_kafka_op_err(
                            rktp->rktp_rkt->rkt_rk, RD_KAFKA_RESP_ERR__FS,
                            "%s [%" PRId32
                            "]: "
                            "Failed to write offset %" PRId64
                            " to "
                            "offset file %s: %s",
                            rktp->rktp_rkt->rkt_topic->str,
                            rktp->rktp_partition, offset,
                            rktp->rktp_offset_path, rd_strerror(errno));
                        err = RD_KAFKA_RESP_ERR__FS;
                        rd_kafka_offset_file_close(rktp);
                        continue;
                }

                /* Need to flush before truncate to preserve write ordering */
                (void)fflush(rktp->rktp_offset_fp);

                /* Truncate file */
#ifdef _WIN32
                if (_chsize_s(_fileno(rktp->rktp_offset_fp), len) == -1)
                        ; /* Ignore truncate failures */
#else
                if (ftruncate(fileno(rktp->rktp_offset_fp), len) == -1)
                        ; /* Ignore truncate failures */
#endif
                rd_kafka_dbg(rktp->rktp_rkt->rkt_rk, TOPIC, "OFFSET",
                             "%s [%" PRId32 "]: wrote offset %" PRId64
                             " to "
                             "file %s",
                             rktp->rktp_rkt->rkt_topic->str,
                             rktp->rktp_partition, offset,
                             rktp->rktp_offset_path);

                rktp->rktp_committed_offset = offset;

                /* If sync interval is set to immediate we sync right away. */
                if (rkt->rkt_conf.offset_store_sync_interval_ms == 0)
                        rd_kafka_offset_file_sync(rktp);


                return RD_KAFKA_RESP_ERR_NO_ERROR;
        }


        return err;
}



/**
 * Commit a list of offsets asynchronously. Response will be queued on 'replyq'.
 * Optional \p cb will be set on requesting op.
 *
 * Makes a copy of \p offsets (may be NULL for current assignment)
 */
static rd_kafka_resp_err_t
rd_kafka_commit0(rd_kafka_t *rk,
                 const rd_kafka_topic_partition_list_t *offsets,
                 rd_kafka_toppar_t *rktp,
                 rd_kafka_replyq_t replyq,
                 void (*cb)(rd_kafka_t *rk,
                            rd_kafka_resp_err_t err,
                            rd_kafka_topic_partition_list_t *offsets,
                            void *opaque),
                 void *opaque,
                 const char *reason) {
        rd_kafka_cgrp_t *rkcg;
        rd_kafka_op_t *rko;

        if (!(rkcg = rd_kafka_cgrp_get(rk)))
                return RD_KAFKA_RESP_ERR__UNKNOWN_GROUP;

        rko = rd_kafka_op_new(RD_KAFKA_OP_OFFSET_COMMIT);
        rko->rko_u.offset_commit.reason = rd_strdup(reason);
        rko->rko_replyq                 = replyq;
        rko->rko_u.offset_commit.cb     = cb;
        rko->rko_u.offset_commit.opaque = opaque;
        if (rktp)
                rko->rko_rktp = rd_kafka_toppar_keep(rktp);

        if (offsets)
                rko->rko_u.offset_commit.partitions =
                    rd_kafka_topic_partition_list_copy(offsets);

        rd_kafka_q_enq(rkcg->rkcg_ops, rko);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}



/**
 * NOTE: 'offsets' may be NULL, see official documentation.
 */
rd_kafka_resp_err_t
rd_kafka_commit(rd_kafka_t *rk,
                const rd_kafka_topic_partition_list_t *offsets,
                int async) {
        rd_kafka_cgrp_t *rkcg;
        rd_kafka_resp_err_t err;
        rd_kafka_q_t *repq   = NULL;
        rd_kafka_replyq_t rq = RD_KAFKA_NO_REPLYQ;

        if (!(rkcg = rd_kafka_cgrp_get(rk)))
                return RD_KAFKA_RESP_ERR__UNKNOWN_GROUP;

        if (!async) {
                repq = rd_kafka_q_new(rk);
                rq   = RD_KAFKA_REPLYQ(repq, 0);
        }

        err = rd_kafka_commit0(rk, offsets, NULL, rq, NULL, NULL, "manual");

        if (!err && !async)
                err = rd_kafka_q_wait_result(repq, RD_POLL_INFINITE);

        if (!async)
                rd_kafka_q_destroy_owner(repq);

        return err;
}


rd_kafka_resp_err_t rd_kafka_commit_message(rd_kafka_t *rk,
                                            const rd_kafka_message_t *rkmessage,
                                            int async) {
        rd_kafka_topic_partition_list_t *offsets;
        rd_kafka_topic_partition_t *rktpar;
        rd_kafka_resp_err_t err;

        if (rkmessage->err)
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        offsets = rd_kafka_topic_partition_list_new(1);
        rktpar  = rd_kafka_topic_partition_list_add(
            offsets, rd_kafka_topic_name(rkmessage->rkt), rkmessage->partition);
        rktpar->offset = rkmessage->offset + 1;

        err = rd_kafka_commit(rk, offsets, async);

        rd_kafka_topic_partition_list_destroy(offsets);

        return err;
}



rd_kafka_resp_err_t
rd_kafka_commit_queue(rd_kafka_t *rk,
                      const rd_kafka_topic_partition_list_t *offsets,
                      rd_kafka_queue_t *rkqu,
                      void (*cb)(rd_kafka_t *rk,
                                 rd_kafka_resp_err_t err,
                                 rd_kafka_topic_partition_list_t *offsets,
                                 void *opaque),
                      void *opaque) {
        rd_kafka_q_t *rkq;
        rd_kafka_resp_err_t err;

        if (!rd_kafka_cgrp_get(rk))
                return RD_KAFKA_RESP_ERR__UNKNOWN_GROUP;

        if (rkqu)
                rkq = rkqu->rkqu_q;
        else
                rkq = rd_kafka_q_new(rk);

        err = rd_kafka_commit0(rk, offsets, NULL, RD_KAFKA_REPLYQ(rkq, 0), cb,
                               opaque, "manual");

        if (!rkqu) {
                rd_kafka_op_t *rko = rd_kafka_q_pop_serve(
                    rkq, RD_POLL_INFINITE, 0, RD_KAFKA_Q_CB_FORCE_RETURN, NULL,
                    NULL);
                if (!rko)
                        err = RD_KAFKA_RESP_ERR__TIMED_OUT;
                else {
                        if (cb)
                                cb(rk, rko->rko_err,
                                   rko->rko_u.offset_commit.partitions, opaque);
                        err = rko->rko_err;
                        rd_kafka_op_destroy(rko);
                }

                if (rkqu)
                        rd_kafka_q_destroy(rkq);
                else
                        rd_kafka_q_destroy_owner(rkq);
        }

        return err;
}



/**
 * Called when a broker commit is done.
 *
 * Locality: toppar handler thread
 * Locks: none
 */
static void
rd_kafka_offset_broker_commit_cb(rd_kafka_t *rk,
                                 rd_kafka_resp_err_t err,
                                 rd_kafka_topic_partition_list_t *offsets,
                                 void *opaque) {
        rd_kafka_toppar_t *rktp;
        rd_kafka_topic_partition_t *rktpar;

        if (offsets->cnt == 0) {
                rd_kafka_dbg(rk, TOPIC, "OFFSETCOMMIT",
                             "No offsets to commit (commit_cb)");
                return;
        }

        rktpar = &offsets->elems[0];

        if (!(rktp =
                  rd_kafka_topic_partition_get_toppar(rk, rktpar, rd_false))) {
                rd_kafka_dbg(rk, TOPIC, "OFFSETCOMMIT",
                             "No local partition found for %s [%" PRId32
                             "] "
                             "while parsing OffsetCommit response "
                             "(offset %" PRId64 ", error \"%s\")",
                             rktpar->topic, rktpar->partition, rktpar->offset,
                             rd_kafka_err2str(rktpar->err));
                return;
        }

        if (!err)
                err = rktpar->err;

        rd_kafka_toppar_offset_commit_result(rktp, err, offsets);

        rd_kafka_dbg(rktp->rktp_rkt->rkt_rk, TOPIC, "OFFSET",
                     "%s [%" PRId32 "]: offset %" PRId64 " %scommitted: %s",
                     rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                     rktpar->offset, err ? "not " : "", rd_kafka_err2str(err));

        rktp->rktp_committing_offset = 0;

        rd_kafka_toppar_lock(rktp);
        if (rktp->rktp_flags & RD_KAFKA_TOPPAR_F_OFFSET_STORE_STOPPING)
                rd_kafka_offset_store_term(rktp, err);
        rd_kafka_toppar_unlock(rktp);

        rd_kafka_toppar_destroy(rktp);
}


static rd_kafka_resp_err_t
rd_kafka_offset_broker_commit(rd_kafka_toppar_t *rktp, const char *reason) {
        rd_kafka_topic_partition_list_t *offsets;
        rd_kafka_topic_partition_t *rktpar;

        rd_kafka_assert(rktp->rktp_rkt->rkt_rk, rktp->rktp_cgrp != NULL);
        rd_kafka_assert(rktp->rktp_rkt->rkt_rk,
                        rktp->rktp_flags & RD_KAFKA_TOPPAR_F_OFFSET_STORE);

        rktp->rktp_committing_offset = rktp->rktp_stored_offset;

        offsets = rd_kafka_topic_partition_list_new(1);
        rktpar  = rd_kafka_topic_partition_list_add(
            offsets, rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition);
        rktpar->offset = rktp->rktp_committing_offset;

        rd_kafka_dbg(rktp->rktp_rkt->rkt_rk, TOPIC, "OFFSETCMT",
                     "%.*s [%" PRId32 "]: committing offset %" PRId64 ": %s",
                     RD_KAFKAP_STR_PR(rktp->rktp_rkt->rkt_topic),
                     rktp->rktp_partition, rktp->rktp_committing_offset,
                     reason);

        rd_kafka_commit0(rktp->rktp_rkt->rkt_rk, offsets, rktp,
                         RD_KAFKA_REPLYQ(rktp->rktp_ops, 0),
                         rd_kafka_offset_broker_commit_cb, NULL, reason);

        rd_kafka_topic_partition_list_destroy(offsets);

        return RD_KAFKA_RESP_ERR__IN_PROGRESS;
}



/**
 * Commit offset to backing store.
 * This might be an async operation.
 *
 * Locality: toppar handler thread
 */
static rd_kafka_resp_err_t rd_kafka_offset_commit(rd_kafka_toppar_t *rktp,
                                                  const char *reason) {
        if (1)  // FIXME
                rd_kafka_dbg(
                    rktp->rktp_rkt->rkt_rk, TOPIC, "OFFSET",
                    "%s [%" PRId32
                    "]: commit: "
                    "stored offset %" PRId64 " > committed offset %" PRId64 "?",
                    rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                    rktp->rktp_stored_offset, rktp->rktp_committed_offset);

        /* Already committed */
        if (rktp->rktp_stored_offset <= rktp->rktp_committed_offset)
                return RD_KAFKA_RESP_ERR_NO_ERROR;

        /* Already committing (for async ops) */
        if (rktp->rktp_stored_offset <= rktp->rktp_committing_offset)
                return RD_KAFKA_RESP_ERR__PREV_IN_PROGRESS;

        switch (rktp->rktp_rkt->rkt_conf.offset_store_method) {
        case RD_KAFKA_OFFSET_METHOD_FILE:
                return rd_kafka_offset_file_commit(rktp);
        case RD_KAFKA_OFFSET_METHOD_BROKER:
                return rd_kafka_offset_broker_commit(rktp, reason);
        default:
                /* UNREACHABLE */
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }
}



/**
 * Sync offset backing store. This is only used for METHOD_FILE.
 *
 * Locality: rktp's broker thread.
 */
rd_kafka_resp_err_t rd_kafka_offset_sync(rd_kafka_toppar_t *rktp) {
        switch (rktp->rktp_rkt->rkt_conf.offset_store_method) {
        case RD_KAFKA_OFFSET_METHOD_FILE:
                return rd_kafka_offset_file_sync(rktp);
        default:
                return RD_KAFKA_RESP_ERR__INVALID_ARG;
        }
}


/**
 * Store offset.
 * Typically called from application code.
 *
 * NOTE: No locks must be held.
 */
rd_kafka_resp_err_t rd_kafka_offset_store(rd_kafka_topic_t *app_rkt,
                                          int32_t partition,
                                          int64_t offset) {
        rd_kafka_topic_t *rkt = rd_kafka_topic_proper(app_rkt);
        rd_kafka_toppar_t *rktp;
        rd_kafka_resp_err_t err;

        /* Find toppar */
        rd_kafka_topic_rdlock(rkt);
        if (!(rktp = rd_kafka_toppar_get(rkt, partition, 0 /*!ua_on_miss*/))) {
                rd_kafka_topic_rdunlock(rkt);
                return RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION;
        }
        rd_kafka_topic_rdunlock(rkt);

        err = rd_kafka_offset_store0(rktp, offset + 1,
                                     rd_false /* Don't force */, RD_DO_LOCK);

        rd_kafka_toppar_destroy(rktp);

        return err;
}


rd_kafka_resp_err_t
rd_kafka_offsets_store(rd_kafka_t *rk,
                       rd_kafka_topic_partition_list_t *offsets) {
        int i;
        int ok_cnt                   = 0;
        rd_kafka_resp_err_t last_err = RD_KAFKA_RESP_ERR_NO_ERROR;

        if (rk->rk_conf.enable_auto_offset_store)
                return RD_KAFKA_RESP_ERR__INVALID_ARG;

        for (i = 0; i < offsets->cnt; i++) {
                rd_kafka_topic_partition_t *rktpar = &offsets->elems[i];
                rd_kafka_toppar_t *rktp;

                rktp =
                    rd_kafka_topic_partition_get_toppar(rk, rktpar, rd_false);
                if (!rktp) {
                        rktpar->err = RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION;
                        last_err    = rktpar->err;
                        continue;
                }

                rktpar->err = rd_kafka_offset_store0(rktp, rktpar->offset,
                                                     rd_false /* don't force */,
                                                     RD_DO_LOCK);
                rd_kafka_toppar_destroy(rktp);

                if (rktpar->err)
                        last_err = rktpar->err;
                else
                        ok_cnt++;
        }

        return offsets->cnt > 0 && ok_cnt == 0 ? last_err
                                               : RD_KAFKA_RESP_ERR_NO_ERROR;
}



/**
 * Decommissions the use of an offset file for a toppar.
 * The file content will not be touched and the file will not be removed.
 */
static rd_kafka_resp_err_t rd_kafka_offset_file_term(rd_kafka_toppar_t *rktp) {
        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;

        /* Sync offset file if the sync is intervalled (> 0) */
        if (rktp->rktp_rkt->rkt_conf.offset_store_sync_interval_ms > 0) {
                rd_kafka_offset_file_sync(rktp);
                rd_kafka_timer_stop(&rktp->rktp_rkt->rkt_rk->rk_timers,
                                    &rktp->rktp_offset_sync_tmr, 1 /*lock*/);
        }


        rd_kafka_offset_file_close(rktp);

        rd_free(rktp->rktp_offset_path);
        rktp->rktp_offset_path = NULL;

        return err;
}

static rd_kafka_op_res_t rd_kafka_offset_reset_op_cb(rd_kafka_t *rk,
                                                     rd_kafka_q_t *rkq,
                                                     rd_kafka_op_t *rko) {
        rd_kafka_toppar_t *rktp = rko->rko_rktp;
        rd_kafka_toppar_lock(rktp);
        rd_kafka_offset_reset(rktp, rko->rko_u.offset_reset.broker_id,
                              rko->rko_u.offset_reset.offset, rko->rko_err,
                              rko->rko_u.offset_reset.reason);
        rd_kafka_toppar_unlock(rktp);
        return RD_KAFKA_OP_RES_HANDLED;
}

/**
 * @brief Take action when the offset for a toppar is unusable (due to an
 *        error, or offset is logical).
 *
 * @param rktp the toppar
 * @param broker_id Originating broker, if any, else RD_KAFKA_NODEID_UA.
 * @param err_offset a logical offset, or offset corresponding to the error.
 * @param err the error, or RD_KAFKA_RESP_ERR_NO_ERROR if offset is logical.
 * @param reason a reason string for logging.
 *
 * @locality: any. if not main thread, work will be enqued on main thread.
 * @ocks: toppar_lock() MUST be held
 */
void rd_kafka_offset_reset(rd_kafka_toppar_t *rktp,
                           int32_t broker_id,
                           int64_t err_offset,
                           rd_kafka_resp_err_t err,
                           const char *reason) {
        int64_t offset    = RD_KAFKA_OFFSET_INVALID;
        const char *extra = "";

        /* Enqueue op for toppar handler thread if we're on the wrong thread. */
        if (!thrd_is_current(rktp->rktp_rkt->rkt_rk->rk_thread)) {
                rd_kafka_op_t *rko =
                    rd_kafka_op_new(RD_KAFKA_OP_OFFSET_RESET | RD_KAFKA_OP_CB);
                rko->rko_op_cb                    = rd_kafka_offset_reset_op_cb;
                rko->rko_err                      = err;
                rko->rko_rktp                     = rd_kafka_toppar_keep(rktp);
                rko->rko_u.offset_reset.broker_id = broker_id;
                rko->rko_u.offset_reset.offset    = err_offset;
                rko->rko_u.offset_reset.reason    = rd_strdup(reason);
                rd_kafka_q_enq(rktp->rktp_ops, rko);
                return;
        }

        if (err_offset == RD_KAFKA_OFFSET_INVALID || err)
                offset = rktp->rktp_rkt->rkt_conf.auto_offset_reset;
        else
                offset = err_offset;

        if (offset == RD_KAFKA_OFFSET_INVALID) {
                /* Error, auto.offset.reset tells us to error out. */
                if (broker_id != RD_KAFKA_NODEID_UA)
                        rd_kafka_consumer_err(
                            rktp->rktp_fetchq, broker_id,
                            RD_KAFKA_RESP_ERR__AUTO_OFFSET_RESET, 0, NULL, rktp,
                            err_offset, "%s: %s (broker %" PRId32 ")", reason,
                            rd_kafka_err2str(err), broker_id);
                else
                        rd_kafka_consumer_err(
                            rktp->rktp_fetchq, broker_id,
                            RD_KAFKA_RESP_ERR__AUTO_OFFSET_RESET, 0, NULL, rktp,
                            err_offset, "%s: %s", reason,
                            rd_kafka_err2str(err));

                rd_kafka_toppar_set_fetch_state(rktp,
                                                RD_KAFKA_TOPPAR_FETCH_NONE);

        } else if (offset == RD_KAFKA_OFFSET_BEGINNING &&
                   rktp->rktp_lo_offset >= 0) {
                /* Use cached log start from last Fetch if available.
                 * Note: The cached end offset (rktp_ls_offset) can't be
                 *       used here since the End offset is a constantly moving
                 *       target as new messages are produced. */
                extra  = "cached BEGINNING offset ";
                offset = rktp->rktp_lo_offset;
                rd_kafka_toppar_next_offset_handle(rktp, offset);

        } else {
                /* Else query cluster for offset */
                rktp->rktp_query_offset = offset;
                rd_kafka_toppar_set_fetch_state(
                    rktp, RD_KAFKA_TOPPAR_FETCH_OFFSET_QUERY);
        }

        /* Offset resets due to error are logged since they might have quite
         * critical impact. For non-errors, or for auto.offset.reset=error,
         * the reason is simply debug-logged. */
        if (!err || err == RD_KAFKA_RESP_ERR__NO_OFFSET ||
            offset == RD_KAFKA_OFFSET_INVALID)
                rd_kafka_dbg(
                    rktp->rktp_rkt->rkt_rk, TOPIC, "OFFSET",
                    "%s [%" PRId32
                    "]: offset reset (at offset %s, broker %" PRId32
                    ") "
                    "to %s%s: %s: %s",
                    rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                    rd_kafka_offset2str(err_offset), broker_id, extra,
                    rd_kafka_offset2str(offset), reason, rd_kafka_err2str(err));
        else
                rd_kafka_log(
                    rktp->rktp_rkt->rkt_rk, LOG_WARNING, "OFFSET",
                    "%s [%" PRId32
                    "]: offset reset (at offset %s, broker %" PRId32
                    ") "
                    "to %s%s: %s: %s",
                    rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                    rd_kafka_offset2str(err_offset), broker_id, extra,
                    rd_kafka_offset2str(offset), reason, rd_kafka_err2str(err));

        /* Note: If rktp is not delegated to the leader, then low and high
           offsets will necessarily be cached from the last FETCH request,
           and so this offset query will never occur in that case for
           BEGINNING / END logical offsets. */
        if (rktp->rktp_fetch_state == RD_KAFKA_TOPPAR_FETCH_OFFSET_QUERY)
                rd_kafka_toppar_offset_request(rktp, rktp->rktp_query_offset,
                                               err ? 100 : 0);
}


/**
 * Escape any special characters in filename 'in' and write escaped
 * string to 'out' (of max size out_size).
 */
static char *mk_esc_filename(const char *in, char *out, size_t out_size) {
        const char *s = in;
        char *o       = out;

        while (*s) {
                const char *esc;
                size_t esclen;

                switch (*s) {
                case '/': /* linux */
                        esc    = "%2F";
                        esclen = strlen(esc);
                        break;
                case ':': /* osx, windows */
                        esc    = "%3A";
                        esclen = strlen(esc);
                        break;
                case '\\': /* windows */
                        esc    = "%5C";
                        esclen = strlen(esc);
                        break;
                default:
                        esc    = s;
                        esclen = 1;
                        break;
                }

                if ((size_t)((o + esclen + 1) - out) >= out_size) {
                        /* No more space in output string, truncate. */
                        break;
                }

                while (esclen-- > 0)
                        *(o++) = *(esc++);

                s++;
        }

        *o = '\0';
        return out;
}


static void rd_kafka_offset_sync_tmr_cb(rd_kafka_timers_t *rkts, void *arg) {
        rd_kafka_toppar_t *rktp = arg;
        rd_kafka_offset_sync(rktp);
}


/**
 * Prepare a toppar for using an offset file.
 *
 * Locality: rdkafka main thread
 * Locks: toppar_lock(rktp) must be held
 */
static void rd_kafka_offset_file_init(rd_kafka_toppar_t *rktp) {
        char spath[4096 + 1]; /* larger than escfile to avoid warning */
        const char *path = rktp->rktp_rkt->rkt_conf.offset_store_path;
        int64_t offset   = RD_KAFKA_OFFSET_INVALID;

        if (rd_kafka_path_is_dir(path)) {
                char tmpfile[1024];
                char escfile[4096];

                /* Include group.id in filename if configured. */
                if (!RD_KAFKAP_STR_IS_NULL(rktp->rktp_rkt->rkt_rk->rk_group_id))
                        rd_snprintf(tmpfile, sizeof(tmpfile),
                                    "%s-%" PRId32 "-%.*s.offset",
                                    rktp->rktp_rkt->rkt_topic->str,
                                    rktp->rktp_partition,
                                    RD_KAFKAP_STR_PR(
                                        rktp->rktp_rkt->rkt_rk->rk_group_id));
                else
                        rd_snprintf(tmpfile, sizeof(tmpfile),
                                    "%s-%" PRId32 ".offset",
                                    rktp->rktp_rkt->rkt_topic->str,
                                    rktp->rktp_partition);

                /* Escape filename to make it safe. */
                mk_esc_filename(tmpfile, escfile, sizeof(escfile));

                rd_snprintf(spath, sizeof(spath), "%s%s%s", path,
                            path[strlen(path) - 1] == '/' ? "" : "/", escfile);

                path = spath;
        }

        rd_kafka_dbg(rktp->rktp_rkt->rkt_rk, TOPIC, "OFFSET",
                     "%s [%" PRId32 "]: using offset file %s",
                     rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                     path);
        rktp->rktp_offset_path = rd_strdup(path);


        /* Set up the offset file sync interval. */
        if (rktp->rktp_rkt->rkt_conf.offset_store_sync_interval_ms > 0)
                rd_kafka_timer_start(
                    &rktp->rktp_rkt->rkt_rk->rk_timers,
                    &rktp->rktp_offset_sync_tmr,
                    rktp->rktp_rkt->rkt_conf.offset_store_sync_interval_ms *
                        1000ll,
                    rd_kafka_offset_sync_tmr_cb, rktp);

        if (rd_kafka_offset_file_open(rktp) != -1) {
                /* Read offset from offset file. */
                offset = rd_kafka_offset_file_read(rktp);
        }

        if (offset != RD_KAFKA_OFFSET_INVALID) {
                /* Start fetching from offset */
                rktp->rktp_stored_offset    = offset;
                rktp->rktp_committed_offset = offset;
                rd_kafka_toppar_next_offset_handle(rktp, offset);

        } else {
                /* Offset was not usable: perform offset reset logic */
                rktp->rktp_committed_offset = RD_KAFKA_OFFSET_INVALID;
                rd_kafka_offset_reset(
                    rktp, RD_KAFKA_NODEID_UA, RD_KAFKA_OFFSET_INVALID,
                    RD_KAFKA_RESP_ERR__FS, "non-readable offset file");
        }
}



/**
 * Terminate broker offset store
 */
static rd_kafka_resp_err_t
rd_kafka_offset_broker_term(rd_kafka_toppar_t *rktp) {
        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * Prepare a toppar for using broker offset commit (broker 0.8.2 or later).
 * When using KafkaConsumer (high-level consumer) this functionality is
 * disabled in favour of the cgrp commits for the entire set of subscriptions.
 */
static void rd_kafka_offset_broker_init(rd_kafka_toppar_t *rktp) {
        if (!rd_kafka_is_simple_consumer(rktp->rktp_rkt->rkt_rk))
                return;
        rd_kafka_offset_reset(rktp, RD_KAFKA_NODEID_UA, RD_KAFKA_OFFSET_STORED,
                              RD_KAFKA_RESP_ERR_NO_ERROR,
                              "query broker for offsets");
}


/**
 * Terminates toppar's offset store, this is the finalizing step after
 * offset_store_stop().
 *
 * Locks: rd_kafka_toppar_lock() MUST be held.
 */
void rd_kafka_offset_store_term(rd_kafka_toppar_t *rktp,
                                rd_kafka_resp_err_t err) {
        rd_kafka_resp_err_t err2;

        rd_kafka_dbg(rktp->rktp_rkt->rkt_rk, TOPIC, "STORETERM",
                     "%s [%" PRId32 "]: offset store terminating",
                     rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition);

        rktp->rktp_flags &= ~RD_KAFKA_TOPPAR_F_OFFSET_STORE_STOPPING;

        rd_kafka_timer_stop(&rktp->rktp_rkt->rkt_rk->rk_timers,
                            &rktp->rktp_offset_commit_tmr, 1 /*lock*/);

        switch (rktp->rktp_rkt->rkt_conf.offset_store_method) {
        case RD_KAFKA_OFFSET_METHOD_FILE:
                err2 = rd_kafka_offset_file_term(rktp);
                break;
        case RD_KAFKA_OFFSET_METHOD_BROKER:
                err2 = rd_kafka_offset_broker_term(rktp);
                break;
        case RD_KAFKA_OFFSET_METHOD_NONE:
                err2 = RD_KAFKA_RESP_ERR_NO_ERROR;
                break;
        }

        /* Prioritize the input error (probably from commit), fall
         * back on termination error. */
        if (!err)
                err = err2;

        rd_kafka_toppar_fetch_stopped(rktp, err);
}


/**
 * Stop toppar's offset store, committing the final offsets, etc.
 *
 * Returns RD_KAFKA_RESP_ERR_NO_ERROR on success,
 * RD_KAFKA_RESP_ERR__IN_PROGRESS if the term triggered an
 * async operation (e.g., broker offset commit), or
 * any other error in case of immediate failure.
 *
 * The offset layer will call rd_kafka_offset_store_term() when
 * the offset management has been fully stopped for this partition.
 *
 * Locks: rd_kafka_toppar_lock() MUST be held.
 */
rd_kafka_resp_err_t rd_kafka_offset_store_stop(rd_kafka_toppar_t *rktp) {
        rd_kafka_resp_err_t err = RD_KAFKA_RESP_ERR_NO_ERROR;

        if (!(rktp->rktp_flags & RD_KAFKA_TOPPAR_F_OFFSET_STORE))
                goto done;

        rktp->rktp_flags |= RD_KAFKA_TOPPAR_F_OFFSET_STORE_STOPPING;

        rd_kafka_dbg(rktp->rktp_rkt->rkt_rk, TOPIC, "OFFSET",
                     "%s [%" PRId32
                     "]: stopping offset store "
                     "(stored offset %" PRId64 ", committed offset %" PRId64
                     ", EOF offset %" PRId64 ")",
                     rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                     rktp->rktp_stored_offset, rktp->rktp_committed_offset,
                     rktp->rktp_offsets_fin.eof_offset);

        /* Store end offset for empty partitions */
        if (rktp->rktp_rkt->rkt_rk->rk_conf.enable_auto_offset_store &&
            rktp->rktp_stored_offset == RD_KAFKA_OFFSET_INVALID &&
            rktp->rktp_offsets_fin.eof_offset > 0)
                rd_kafka_offset_store0(rktp, rktp->rktp_offsets_fin.eof_offset,
                                       rd_true /* force */, RD_DONT_LOCK);

        /* Commit offset to backing store.
         * This might be an async operation. */
        if (rd_kafka_is_simple_consumer(rktp->rktp_rkt->rkt_rk) &&
            rktp->rktp_stored_offset > rktp->rktp_committed_offset)
                err = rd_kafka_offset_commit(rktp, "offset store stop");

        /* If stop is in progress (async commit), return now. */
        if (err == RD_KAFKA_RESP_ERR__IN_PROGRESS)
                return err;

done:
        /* Stop is done */
        rd_kafka_offset_store_term(rktp, err);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


static void rd_kafka_offset_auto_commit_tmr_cb(rd_kafka_timers_t *rkts,
                                               void *arg) {
        rd_kafka_toppar_t *rktp = arg;
        rd_kafka_offset_commit(rktp, "auto commit timer");
}

void rd_kafka_offset_query_tmr_cb(rd_kafka_timers_t *rkts, void *arg) {
        rd_kafka_toppar_t *rktp = arg;
        rd_kafka_toppar_lock(rktp);
        rd_kafka_dbg(rktp->rktp_rkt->rkt_rk, TOPIC, "OFFSET",
                     "Topic %s [%" PRId32
                     "]: timed offset query for %s in "
                     "state %s",
                     rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                     rd_kafka_offset2str(rktp->rktp_query_offset),
                     rd_kafka_fetch_states[rktp->rktp_fetch_state]);
        rd_kafka_toppar_offset_request(rktp, rktp->rktp_query_offset, 0);
        rd_kafka_toppar_unlock(rktp);
}


/**
 * Initialize toppar's offset store.
 *
 * Locality: toppar handler thread
 */
void rd_kafka_offset_store_init(rd_kafka_toppar_t *rktp) {
        static const char *store_names[] = {"none", "file", "broker"};

        rd_kafka_dbg(rktp->rktp_rkt->rkt_rk, TOPIC, "OFFSET",
                     "%s [%" PRId32 "]: using offset store method: %s",
                     rktp->rktp_rkt->rkt_topic->str, rktp->rktp_partition,
                     store_names[rktp->rktp_rkt->rkt_conf.offset_store_method]);

        /* The committed offset is unknown at this point. */
        rktp->rktp_committed_offset = RD_KAFKA_OFFSET_INVALID;

        /* Set up the commit interval (for simple consumer). */
        if (rd_kafka_is_simple_consumer(rktp->rktp_rkt->rkt_rk) &&
            rktp->rktp_rkt->rkt_conf.auto_commit_interval_ms > 0)
                rd_kafka_timer_start(
                    &rktp->rktp_rkt->rkt_rk->rk_timers,
                    &rktp->rktp_offset_commit_tmr,
                    rktp->rktp_rkt->rkt_conf.auto_commit_interval_ms * 1000ll,
                    rd_kafka_offset_auto_commit_tmr_cb, rktp);

        switch (rktp->rktp_rkt->rkt_conf.offset_store_method) {
        case RD_KAFKA_OFFSET_METHOD_FILE:
                rd_kafka_offset_file_init(rktp);
                break;
        case RD_KAFKA_OFFSET_METHOD_BROKER:
                rd_kafka_offset_broker_init(rktp);
                break;
        case RD_KAFKA_OFFSET_METHOD_NONE:
                break;
        default:
                /* NOTREACHED */
                return;
        }

        rktp->rktp_flags |= RD_KAFKA_TOPPAR_F_OFFSET_STORE;
}
