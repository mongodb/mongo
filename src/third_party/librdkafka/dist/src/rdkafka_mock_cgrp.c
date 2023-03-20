/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2020 Magnus Edenhill
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

/**
 * Mocks
 *
 */

#include "rdkafka_int.h"
#include "rdbuf.h"
#include "rdkafka_mock_int.h"


static const char *rd_kafka_mock_cgrp_state_names[] = {
    "Empty", "Joining", "Syncing", "Rebalancing", "Up"};


static void rd_kafka_mock_cgrp_rebalance(rd_kafka_mock_cgrp_t *mcgrp,
                                         const char *reason);
static void
rd_kafka_mock_cgrp_member_destroy(rd_kafka_mock_cgrp_t *mcgrp,
                                  rd_kafka_mock_cgrp_member_t *member);

static void rd_kafka_mock_cgrp_set_state(rd_kafka_mock_cgrp_t *mcgrp,
                                         unsigned int new_state,
                                         const char *reason) {
        if (mcgrp->state == new_state)
                return;

        rd_kafka_dbg(mcgrp->cluster->rk, MOCK, "MOCK",
                     "Mock consumer group %s with %d member(s) "
                     "changing state %s -> %s: %s",
                     mcgrp->id, mcgrp->member_cnt,
                     rd_kafka_mock_cgrp_state_names[mcgrp->state],
                     rd_kafka_mock_cgrp_state_names[new_state], reason);

        mcgrp->state = new_state;
}


/**
 * @brief Mark member as active (restart session timer)
 */
void rd_kafka_mock_cgrp_member_active(rd_kafka_mock_cgrp_t *mcgrp,
                                      rd_kafka_mock_cgrp_member_t *member) {
        rd_kafka_dbg(mcgrp->cluster->rk, MOCK, "MOCK",
                     "Marking mock consumer group member %s as active",
                     member->id);
        member->ts_last_activity = rd_clock();
}


/**
 * @brief Verify that the protocol request is valid in the current state.
 *
 * @param member may be NULL.
 */
rd_kafka_resp_err_t
rd_kafka_mock_cgrp_check_state(rd_kafka_mock_cgrp_t *mcgrp,
                               rd_kafka_mock_cgrp_member_t *member,
                               const rd_kafka_buf_t *request,
                               int32_t generation_id) {
        int16_t ApiKey              = request->rkbuf_reqhdr.ApiKey;
        rd_bool_t has_generation_id = ApiKey == RD_KAFKAP_SyncGroup ||
                                      ApiKey == RD_KAFKAP_Heartbeat ||
                                      ApiKey == RD_KAFKAP_OffsetCommit;

        if (has_generation_id && generation_id != mcgrp->generation_id)
                return RD_KAFKA_RESP_ERR_ILLEGAL_GENERATION;

        if (ApiKey == RD_KAFKAP_OffsetCommit && !member)
                return RD_KAFKA_RESP_ERR_UNKNOWN_MEMBER_ID;

        switch (mcgrp->state) {
        case RD_KAFKA_MOCK_CGRP_STATE_EMPTY:
                if (ApiKey == RD_KAFKAP_JoinGroup)
                        return RD_KAFKA_RESP_ERR_NO_ERROR;
                break;

        case RD_KAFKA_MOCK_CGRP_STATE_JOINING:
                if (ApiKey == RD_KAFKAP_JoinGroup ||
                    ApiKey == RD_KAFKAP_LeaveGroup)
                        return RD_KAFKA_RESP_ERR_NO_ERROR;
                else
                        return RD_KAFKA_RESP_ERR_REBALANCE_IN_PROGRESS;

        case RD_KAFKA_MOCK_CGRP_STATE_SYNCING:
                if (ApiKey == RD_KAFKAP_SyncGroup ||
                    ApiKey == RD_KAFKAP_JoinGroup ||
                    ApiKey == RD_KAFKAP_LeaveGroup)
                        return RD_KAFKA_RESP_ERR_NO_ERROR;
                else
                        return RD_KAFKA_RESP_ERR_REBALANCE_IN_PROGRESS;

        case RD_KAFKA_MOCK_CGRP_STATE_REBALANCING:
                if (ApiKey == RD_KAFKAP_JoinGroup ||
                    ApiKey == RD_KAFKAP_LeaveGroup ||
                    ApiKey == RD_KAFKAP_OffsetCommit)
                        return RD_KAFKA_RESP_ERR_NO_ERROR;
                else
                        return RD_KAFKA_RESP_ERR_REBALANCE_IN_PROGRESS;

        case RD_KAFKA_MOCK_CGRP_STATE_UP:
                if (ApiKey == RD_KAFKAP_JoinGroup ||
                    ApiKey == RD_KAFKAP_LeaveGroup ||
                    ApiKey == RD_KAFKAP_Heartbeat ||
                    ApiKey == RD_KAFKAP_OffsetCommit)
                        return RD_KAFKA_RESP_ERR_NO_ERROR;
                break;
        }

        return RD_KAFKA_RESP_ERR_INVALID_REQUEST;
}


/**
 * @brief Set a member's assignment (from leader's SyncGroupRequest)
 */
void rd_kafka_mock_cgrp_member_assignment_set(
    rd_kafka_mock_cgrp_t *mcgrp,
    rd_kafka_mock_cgrp_member_t *member,
    const rd_kafkap_bytes_t *Metadata) {
        if (member->assignment) {
                rd_assert(mcgrp->assignment_cnt > 0);
                mcgrp->assignment_cnt--;
                rd_kafkap_bytes_destroy(member->assignment);
                member->assignment = NULL;
        }

        if (Metadata) {
                mcgrp->assignment_cnt++;
                member->assignment = rd_kafkap_bytes_copy(Metadata);
        }
}


/**
 * @brief Sync done (successfully) or failed, send responses back to members.
 */
static void rd_kafka_mock_cgrp_sync_done(rd_kafka_mock_cgrp_t *mcgrp,
                                         rd_kafka_resp_err_t err) {
        rd_kafka_mock_cgrp_member_t *member;

        TAILQ_FOREACH(member, &mcgrp->members, link) {
                rd_kafka_buf_t *resp;

                if ((resp = member->resp)) {
                        member->resp = NULL;
                        rd_assert(resp->rkbuf_reqhdr.ApiKey ==
                                  RD_KAFKAP_SyncGroup);

                        rd_kafka_buf_write_i16(resp, err); /* ErrorCode */
                        /* MemberState */
                        rd_kafka_buf_write_kbytes(
                            resp, !err ? member->assignment : NULL);
                }

                rd_kafka_mock_cgrp_member_assignment_set(mcgrp, member, NULL);

                if (member->conn) {
                        rd_kafka_mock_connection_set_blocking(member->conn,
                                                              rd_false);
                        if (resp)
                                rd_kafka_mock_connection_send_response(
                                    member->conn, resp);
                } else if (resp) {
                        /* Member has disconnected. */
                        rd_kafka_buf_destroy(resp);
                }
        }
}


/**
 * @brief Check if all members have sent SyncGroupRequests, if so, propagate
 *        assignment to members.
 */
static void rd_kafka_mock_cgrp_sync_check(rd_kafka_mock_cgrp_t *mcgrp) {

        rd_kafka_dbg(mcgrp->cluster->rk, MOCK, "MOCK",
                     "Mock consumer group %s: awaiting %d/%d syncing members "
                     "in state %s",
                     mcgrp->id, mcgrp->assignment_cnt, mcgrp->member_cnt,
                     rd_kafka_mock_cgrp_state_names[mcgrp->state]);

        if (mcgrp->assignment_cnt < mcgrp->member_cnt)
                return;

        rd_kafka_mock_cgrp_sync_done(mcgrp, RD_KAFKA_RESP_ERR_NO_ERROR);
        rd_kafka_mock_cgrp_set_state(mcgrp, RD_KAFKA_MOCK_CGRP_STATE_UP,
                                     "all members synced");
}


/**
 * @brief Member has sent SyncGroupRequest and is waiting for a response,
 *        which will be sent when the all group member SyncGroupRequest are
 *        received.
 */
rd_kafka_resp_err_t
rd_kafka_mock_cgrp_member_sync_set(rd_kafka_mock_cgrp_t *mcgrp,
                                   rd_kafka_mock_cgrp_member_t *member,
                                   rd_kafka_mock_connection_t *mconn,
                                   rd_kafka_buf_t *resp) {

        if (mcgrp->state != RD_KAFKA_MOCK_CGRP_STATE_SYNCING)
                return RD_KAFKA_RESP_ERR_REBALANCE_IN_PROGRESS; /* FIXME */

        rd_kafka_mock_cgrp_member_active(mcgrp, member);

        rd_assert(!member->resp);

        member->resp = resp;
        member->conn = mconn;
        rd_kafka_mock_connection_set_blocking(member->conn, rd_true);

        /* Check if all members now have an assignment, if so, send responses */
        rd_kafka_mock_cgrp_sync_check(mcgrp);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Member is explicitly leaving the group (through LeaveGroupRequest)
 */
rd_kafka_resp_err_t
rd_kafka_mock_cgrp_member_leave(rd_kafka_mock_cgrp_t *mcgrp,
                                rd_kafka_mock_cgrp_member_t *member) {

        rd_kafka_dbg(mcgrp->cluster->rk, MOCK, "MOCK",
                     "Member %s is leaving group %s", member->id, mcgrp->id);

        rd_kafka_mock_cgrp_member_destroy(mcgrp, member);

        rd_kafka_mock_cgrp_rebalance(mcgrp, "explicit member leave");

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Destroys/frees an array of protocols, including the array itself.
 */
void rd_kafka_mock_cgrp_protos_destroy(rd_kafka_mock_cgrp_proto_t *protos,
                                       int proto_cnt) {
        int i;

        for (i = 0; i < proto_cnt; i++) {
                rd_free(protos[i].name);
                if (protos[i].metadata)
                        rd_free(protos[i].metadata);
        }

        rd_free(protos);
}

static void
rd_kafka_mock_cgrp_rebalance_timer_restart(rd_kafka_mock_cgrp_t *mcgrp,
                                           int timeout_ms);

/**
 * @brief Elect consumer group leader and send JoinGroup responses
 */
static void rd_kafka_mock_cgrp_elect_leader(rd_kafka_mock_cgrp_t *mcgrp) {
        rd_kafka_mock_cgrp_member_t *member;

        rd_assert(mcgrp->state == RD_KAFKA_MOCK_CGRP_STATE_JOINING);
        rd_assert(!TAILQ_EMPTY(&mcgrp->members));

        mcgrp->generation_id++;

        /* Elect a leader.
         * FIXME: For now we'll use the first member */
        mcgrp->leader = TAILQ_FIRST(&mcgrp->members);

        rd_kafka_dbg(mcgrp->cluster->rk, MOCK, "MOCK",
                     "Consumer group %s with %d member(s) is rebalancing: "
                     "elected leader is %s, generation id %d",
                     mcgrp->id, mcgrp->member_cnt, mcgrp->leader->id,
                     mcgrp->generation_id);

        /* Find the most commonly supported protocol name among the members.
         * FIXME: For now we'll blindly use the first protocol of the leader. */
        if (mcgrp->protocol_name)
                rd_free(mcgrp->protocol_name);
        mcgrp->protocol_name = RD_KAFKAP_STR_DUP(mcgrp->leader->protos[0].name);

        /* Send JoinGroupResponses to all members */
        TAILQ_FOREACH(member, &mcgrp->members, link) {
                rd_bool_t is_leader = member == mcgrp->leader;
                int member_cnt      = is_leader ? mcgrp->member_cnt : 0;
                rd_kafka_buf_t *resp;
                rd_kafka_mock_cgrp_member_t *member2;
                rd_kafka_mock_connection_t *mconn;

                /* Member connection has been closed, it will eventually
                 * reconnect or time out from the group. */
                if (!member->conn || !member->resp)
                        continue;
                mconn        = member->conn;
                member->conn = NULL;
                resp         = member->resp;
                member->resp = NULL;

                rd_assert(resp->rkbuf_reqhdr.ApiKey == RD_KAFKAP_JoinGroup);

                rd_kafka_buf_write_i16(resp, 0); /* ErrorCode */
                rd_kafka_buf_write_i32(resp, mcgrp->generation_id);
                rd_kafka_buf_write_str(resp, mcgrp->protocol_name, -1);
                rd_kafka_buf_write_str(resp, mcgrp->leader->id, -1);
                rd_kafka_buf_write_str(resp, member->id, -1);
                rd_kafka_buf_write_i32(resp, member_cnt);

                /* Send full member list to leader */
                if (member_cnt > 0) {
                        TAILQ_FOREACH(member2, &mcgrp->members, link) {
                                rd_kafka_buf_write_str(resp, member2->id, -1);
                                if (resp->rkbuf_reqhdr.ApiVersion >= 5)
                                        rd_kafka_buf_write_str(
                                            resp, member2->group_instance_id,
                                            -1);
                                /* FIXME: look up correct protocol name */
                                rd_assert(!rd_kafkap_str_cmp_str(
                                    member2->protos[0].name,
                                    mcgrp->protocol_name));

                                rd_kafka_buf_write_kbytes(
                                    resp, member2->protos[0].metadata);
                        }
                }

                /* Mark each member as active to avoid them timing out
                 * at the same time as a JoinGroup handler that blocks
                 * session.timeout.ms to elect a leader. */
                rd_kafka_mock_cgrp_member_active(mcgrp, member);

                rd_kafka_mock_connection_set_blocking(mconn, rd_false);
                rd_kafka_mock_connection_send_response(mconn, resp);
        }

        mcgrp->last_member_cnt = mcgrp->member_cnt;

        rd_kafka_mock_cgrp_set_state(mcgrp, RD_KAFKA_MOCK_CGRP_STATE_SYNCING,
                                     "leader elected, waiting for all "
                                     "members to sync");

        rd_kafka_mock_cgrp_rebalance_timer_restart(mcgrp,
                                                   mcgrp->session_timeout_ms);
}


/**
 * @brief Trigger group rebalance.
 */
static void rd_kafka_mock_cgrp_rebalance(rd_kafka_mock_cgrp_t *mcgrp,
                                         const char *reason) {
        int timeout_ms;

        if (mcgrp->state == RD_KAFKA_MOCK_CGRP_STATE_JOINING)
                return; /* Do nothing, group is already rebalancing. */
        else if (mcgrp->state == RD_KAFKA_MOCK_CGRP_STATE_EMPTY)
                timeout_ms = 3000; /* First join, low timeout.
                                    * Same as group.initial.rebalance.delay.ms
                                    * on the broker. */
        else if (mcgrp->state == RD_KAFKA_MOCK_CGRP_STATE_REBALANCING &&
                 mcgrp->member_cnt == mcgrp->last_member_cnt)
                timeout_ms = 100; /* All members rejoined, quickly transition
                                   * to election. */
        else /* Let the rebalance delay be a bit shorter than the
              * session timeout so that we don't time out waiting members
              * who are also subject to the session timeout. */
                timeout_ms = mcgrp->session_timeout_ms > 1000
                                 ? mcgrp->session_timeout_ms - 1000
                                 : mcgrp->session_timeout_ms;

        if (mcgrp->state == RD_KAFKA_MOCK_CGRP_STATE_SYNCING)
                /* Abort current Syncing state */
                rd_kafka_mock_cgrp_sync_done(
                    mcgrp, RD_KAFKA_RESP_ERR_REBALANCE_IN_PROGRESS);

        rd_kafka_mock_cgrp_set_state(mcgrp, RD_KAFKA_MOCK_CGRP_STATE_JOINING,
                                     reason);
        rd_kafka_mock_cgrp_rebalance_timer_restart(mcgrp, timeout_ms);
}

/**
 * @brief Consumer group state machine triggered by timer events.
 */
static void rd_kafka_mock_cgrp_fsm_timeout(rd_kafka_mock_cgrp_t *mcgrp) {
        rd_kafka_dbg(mcgrp->cluster->rk, MOCK, "MOCK",
                     "Mock consumer group %s FSM timeout in state %s",
                     mcgrp->id, rd_kafka_mock_cgrp_state_names[mcgrp->state]);

        switch (mcgrp->state) {
        case RD_KAFKA_MOCK_CGRP_STATE_EMPTY:
                /* No members, do nothing */
                break;
        case RD_KAFKA_MOCK_CGRP_STATE_JOINING:
                /* Timed out waiting for more members, elect a leader */
                if (mcgrp->member_cnt > 0)
                        rd_kafka_mock_cgrp_elect_leader(mcgrp);
                else
                        rd_kafka_mock_cgrp_set_state(
                            mcgrp, RD_KAFKA_MOCK_CGRP_STATE_EMPTY,
                            "no members joined");
                break;

        case RD_KAFKA_MOCK_CGRP_STATE_SYNCING:
                /* Timed out waiting for all members to sync */

                /* Send error response to all waiting members */
                rd_kafka_mock_cgrp_sync_done(
                    mcgrp, RD_KAFKA_RESP_ERR_REBALANCE_IN_PROGRESS /* FIXME */);

                rd_kafka_mock_cgrp_set_state(
                    mcgrp, RD_KAFKA_MOCK_CGRP_STATE_REBALANCING,
                    "timed out waiting for all members to synchronize");
                break;

        case RD_KAFKA_MOCK_CGRP_STATE_REBALANCING:
                /* Timed out waiting for all members to Leave or re-Join */
                rd_kafka_mock_cgrp_set_state(mcgrp,
                                             RD_KAFKA_MOCK_CGRP_STATE_JOINING,
                                             "timed out waiting for all "
                                             "members to re-Join or Leave");
                break;

        case RD_KAFKA_MOCK_CGRP_STATE_UP:
                /* No fsm timers triggered in this state, see
                 * the session_tmr instead */
                break;
        }
}

static void rd_kafka_mcgrp_rebalance_timer_cb(rd_kafka_timers_t *rkts,
                                              void *arg) {
        rd_kafka_mock_cgrp_t *mcgrp = arg;

        rd_kafka_mock_cgrp_fsm_timeout(mcgrp);
}


/**
 * @brief Restart the rebalance timer, postponing leader election.
 */
static void
rd_kafka_mock_cgrp_rebalance_timer_restart(rd_kafka_mock_cgrp_t *mcgrp,
                                           int timeout_ms) {
        rd_kafka_timer_start_oneshot(
            &mcgrp->cluster->timers, &mcgrp->rebalance_tmr, rd_true,
            timeout_ms * 1000, rd_kafka_mcgrp_rebalance_timer_cb, mcgrp);
}


static void
rd_kafka_mock_cgrp_member_destroy(rd_kafka_mock_cgrp_t *mcgrp,
                                  rd_kafka_mock_cgrp_member_t *member) {
        rd_assert(mcgrp->member_cnt > 0);
        TAILQ_REMOVE(&mcgrp->members, member, link);
        mcgrp->member_cnt--;

        rd_free(member->id);

        if (member->resp)
                rd_kafka_buf_destroy(member->resp);

        if (member->group_instance_id)
                rd_free(member->group_instance_id);

        rd_kafka_mock_cgrp_member_assignment_set(mcgrp, member, NULL);

        rd_kafka_mock_cgrp_protos_destroy(member->protos, member->proto_cnt);

        rd_free(member);
}


/**
 * @brief Find member in group.
 */
rd_kafka_mock_cgrp_member_t *
rd_kafka_mock_cgrp_member_find(const rd_kafka_mock_cgrp_t *mcgrp,
                               const rd_kafkap_str_t *MemberId) {
        const rd_kafka_mock_cgrp_member_t *member;
        TAILQ_FOREACH(member, &mcgrp->members, link) {
                if (!rd_kafkap_str_cmp_str(MemberId, member->id))
                        return (rd_kafka_mock_cgrp_member_t *)member;
        }

        return NULL;
}


/**
 * @brief Update or add member to consumer group
 */
rd_kafka_resp_err_t
rd_kafka_mock_cgrp_member_add(rd_kafka_mock_cgrp_t *mcgrp,
                              rd_kafka_mock_connection_t *mconn,
                              rd_kafka_buf_t *resp,
                              const rd_kafkap_str_t *MemberId,
                              const rd_kafkap_str_t *ProtocolType,
                              rd_kafka_mock_cgrp_proto_t *protos,
                              int proto_cnt,
                              int session_timeout_ms) {
        rd_kafka_mock_cgrp_member_t *member;
        rd_kafka_resp_err_t err;

        err = rd_kafka_mock_cgrp_check_state(mcgrp, NULL, resp, -1);
        if (err)
                return err;

        /* Find member */
        member = rd_kafka_mock_cgrp_member_find(mcgrp, MemberId);
        if (!member) {
                /* Not found, add member */
                member = rd_calloc(1, sizeof(*member));

                if (!RD_KAFKAP_STR_LEN(MemberId)) {
                        /* Generate a member id */
                        char memberid[32];
                        rd_snprintf(memberid, sizeof(memberid), "%p", member);
                        member->id = rd_strdup(memberid);
                } else
                        member->id = RD_KAFKAP_STR_DUP(MemberId);

                TAILQ_INSERT_TAIL(&mcgrp->members, member, link);
                mcgrp->member_cnt++;
        }

        if (mcgrp->state != RD_KAFKA_MOCK_CGRP_STATE_JOINING)
                rd_kafka_mock_cgrp_rebalance(mcgrp, "member join");

        mcgrp->session_timeout_ms = session_timeout_ms;

        if (member->protos)
                rd_kafka_mock_cgrp_protos_destroy(member->protos,
                                                  member->proto_cnt);
        member->protos    = protos;
        member->proto_cnt = proto_cnt;

        rd_assert(!member->resp);
        member->resp = resp;
        member->conn = mconn;
        rd_kafka_mock_cgrp_member_active(mcgrp, member);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Check if any members have exceeded the session timeout.
 */
static void rd_kafka_mock_cgrp_session_tmr_cb(rd_kafka_timers_t *rkts,
                                              void *arg) {
        rd_kafka_mock_cgrp_t *mcgrp = arg;
        rd_kafka_mock_cgrp_member_t *member, *tmp;
        rd_ts_t now     = rd_clock();
        int timeout_cnt = 0;

        TAILQ_FOREACH_SAFE(member, &mcgrp->members, link, tmp) {
                if (member->ts_last_activity +
                        (mcgrp->session_timeout_ms * 1000) >
                    now)
                        continue;

                rd_kafka_dbg(mcgrp->cluster->rk, MOCK, "MOCK",
                             "Member %s session timed out for group %s",
                             member->id, mcgrp->id);

                rd_kafka_mock_cgrp_member_destroy(mcgrp, member);
                timeout_cnt++;
        }

        if (timeout_cnt)
                rd_kafka_mock_cgrp_rebalance(mcgrp, "member timeout");
}


void rd_kafka_mock_cgrp_destroy(rd_kafka_mock_cgrp_t *mcgrp) {
        rd_kafka_mock_cgrp_member_t *member;

        TAILQ_REMOVE(&mcgrp->cluster->cgrps, mcgrp, link);

        rd_kafka_timer_stop(&mcgrp->cluster->timers, &mcgrp->rebalance_tmr,
                            rd_true);
        rd_kafka_timer_stop(&mcgrp->cluster->timers, &mcgrp->session_tmr,
                            rd_true);
        rd_free(mcgrp->id);
        rd_free(mcgrp->protocol_type);
        if (mcgrp->protocol_name)
                rd_free(mcgrp->protocol_name);
        while ((member = TAILQ_FIRST(&mcgrp->members)))
                rd_kafka_mock_cgrp_member_destroy(mcgrp, member);
        rd_free(mcgrp);
}


rd_kafka_mock_cgrp_t *rd_kafka_mock_cgrp_find(rd_kafka_mock_cluster_t *mcluster,
                                              const rd_kafkap_str_t *GroupId) {
        rd_kafka_mock_cgrp_t *mcgrp;
        TAILQ_FOREACH(mcgrp, &mcluster->cgrps, link) {
                if (!rd_kafkap_str_cmp_str(GroupId, mcgrp->id))
                        return mcgrp;
        }

        return NULL;
}


/**
 * @brief Find or create a consumer group
 */
rd_kafka_mock_cgrp_t *
rd_kafka_mock_cgrp_get(rd_kafka_mock_cluster_t *mcluster,
                       const rd_kafkap_str_t *GroupId,
                       const rd_kafkap_str_t *ProtocolType) {
        rd_kafka_mock_cgrp_t *mcgrp;

        mcgrp = rd_kafka_mock_cgrp_find(mcluster, GroupId);
        if (mcgrp)
                return mcgrp;

        /* FIXME: What to do with mismatching ProtocolTypes? */

        mcgrp = rd_calloc(1, sizeof(*mcgrp));

        mcgrp->cluster       = mcluster;
        mcgrp->id            = RD_KAFKAP_STR_DUP(GroupId);
        mcgrp->protocol_type = RD_KAFKAP_STR_DUP(ProtocolType);
        mcgrp->generation_id = 1;
        TAILQ_INIT(&mcgrp->members);
        rd_kafka_timer_start(&mcluster->timers, &mcgrp->session_tmr,
                             1000 * 1000 /*1s*/,
                             rd_kafka_mock_cgrp_session_tmr_cb, mcgrp);

        TAILQ_INSERT_TAIL(&mcluster->cgrps, mcgrp, link);

        return mcgrp;
}


/**
 * @brief A client connection closed, check if any cgrp has any state
 *        for this connection that needs to be cleared.
 */
void rd_kafka_mock_cgrps_connection_closed(rd_kafka_mock_cluster_t *mcluster,
                                           rd_kafka_mock_connection_t *mconn) {
        rd_kafka_mock_cgrp_t *mcgrp;

        TAILQ_FOREACH(mcgrp, &mcluster->cgrps, link) {
                rd_kafka_mock_cgrp_member_t *member, *tmp;
                TAILQ_FOREACH_SAFE(member, &mcgrp->members, link, tmp) {
                        if (member->conn == mconn) {
                                member->conn = NULL;
                                if (member->resp) {
                                        rd_kafka_buf_destroy(member->resp);
                                        member->resp = NULL;
                                }
                        }
                }
        }
}
