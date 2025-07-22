/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2020-2022, Magnus Edenhill
 *               2023, Confluent Inc.
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


static const char *rd_kafka_mock_cgrp_classic_state_names[] = {
    "Empty", "Joining", "Syncing", "Rebalancing", "Up"};


static void
rd_kafka_mock_cgrp_classic_rebalance(rd_kafka_mock_cgrp_classic_t *mcgrp,
                                     const char *reason);
static void rd_kafka_mock_cgrp_classic_member_destroy(
    rd_kafka_mock_cgrp_classic_t *mcgrp,
    rd_kafka_mock_cgrp_classic_member_t *member);

static void
rd_kafka_mock_cgrp_classic_set_state(rd_kafka_mock_cgrp_classic_t *mcgrp,
                                     unsigned int new_state,
                                     const char *reason) {
        if (mcgrp->state == new_state)
                return;

        rd_kafka_dbg(mcgrp->cluster->rk, MOCK, "MOCK",
                     "Mock consumer group %s with %d member(s) "
                     "changing state %s -> %s: %s",
                     mcgrp->id, mcgrp->member_cnt,
                     rd_kafka_mock_cgrp_classic_state_names[mcgrp->state],
                     rd_kafka_mock_cgrp_classic_state_names[new_state], reason);

        mcgrp->state = new_state;
}


/**
 * @brief Mark member as active (restart session timer)
 */
void rd_kafka_mock_cgrp_classic_member_active(
    rd_kafka_mock_cgrp_classic_t *mcgrp,
    rd_kafka_mock_cgrp_classic_member_t *member) {
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
rd_kafka_resp_err_t rd_kafka_mock_cgrp_classic_check_state(
    rd_kafka_mock_cgrp_classic_t *mcgrp,
    rd_kafka_mock_cgrp_classic_member_t *member,
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
void rd_kafka_mock_cgrp_classic_member_assignment_set(
    rd_kafka_mock_cgrp_classic_t *mcgrp,
    rd_kafka_mock_cgrp_classic_member_t *member,
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
static void
rd_kafka_mock_cgrp_classic_sync_done(rd_kafka_mock_cgrp_classic_t *mcgrp,
                                     rd_kafka_resp_err_t err) {
        rd_kafka_mock_cgrp_classic_member_t *member;

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

                rd_kafka_mock_cgrp_classic_member_assignment_set(mcgrp, member,
                                                                 NULL);

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
static void
rd_kafka_mock_cgrp_classic_sync_check(rd_kafka_mock_cgrp_classic_t *mcgrp) {

        rd_kafka_dbg(mcgrp->cluster->rk, MOCK, "MOCK",
                     "Mock consumer group %s: awaiting %d/%d syncing members "
                     "in state %s",
                     mcgrp->id, mcgrp->assignment_cnt, mcgrp->member_cnt,
                     rd_kafka_mock_cgrp_classic_state_names[mcgrp->state]);

        if (mcgrp->assignment_cnt < mcgrp->member_cnt)
                return;

        rd_kafka_mock_cgrp_classic_sync_done(mcgrp, RD_KAFKA_RESP_ERR_NO_ERROR);
        rd_kafka_mock_cgrp_classic_set_state(mcgrp, RD_KAFKA_MOCK_CGRP_STATE_UP,
                                             "all members synced");
}


/**
 * @brief Member has sent SyncGroupRequest and is waiting for a response,
 *        which will be sent when the all group member SyncGroupRequest are
 *        received.
 */
rd_kafka_resp_err_t rd_kafka_mock_cgrp_classic_member_sync_set(
    rd_kafka_mock_cgrp_classic_t *mcgrp,
    rd_kafka_mock_cgrp_classic_member_t *member,
    rd_kafka_mock_connection_t *mconn,
    rd_kafka_buf_t *resp) {

        if (mcgrp->state != RD_KAFKA_MOCK_CGRP_STATE_SYNCING)
                return RD_KAFKA_RESP_ERR_REBALANCE_IN_PROGRESS; /* FIXME */

        rd_kafka_mock_cgrp_classic_member_active(mcgrp, member);

        rd_assert(!member->resp);

        member->resp = resp;
        member->conn = mconn;
        rd_kafka_mock_connection_set_blocking(member->conn, rd_true);

        /* Check if all members now have an assignment, if so, send responses */
        rd_kafka_mock_cgrp_classic_sync_check(mcgrp);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}


/**
 * @brief Member is explicitly leaving the group (through LeaveGroupRequest)
 */
rd_kafka_resp_err_t rd_kafka_mock_cgrp_classic_member_leave(
    rd_kafka_mock_cgrp_classic_t *mcgrp,
    rd_kafka_mock_cgrp_classic_member_t *member) {

        rd_kafka_dbg(mcgrp->cluster->rk, MOCK, "MOCK",
                     "Member %s is leaving group %s", member->id, mcgrp->id);

        rd_kafka_mock_cgrp_classic_member_destroy(mcgrp, member);

        rd_kafka_mock_cgrp_classic_rebalance(mcgrp, "explicit member leave");

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Destroys/frees an array of protocols, including the array itself.
 */
void rd_kafka_mock_cgrp_classic_protos_destroy(
    rd_kafka_mock_cgrp_classic_proto_t *protos,
    int proto_cnt) {
        int i;

        for (i = 0; i < proto_cnt; i++) {
                rd_free(protos[i].name);
                if (protos[i].metadata)
                        rd_free(protos[i].metadata);
        }

        rd_free(protos);
}

static void rd_kafka_mock_cgrp_classic_rebalance_timer_restart(
    rd_kafka_mock_cgrp_classic_t *mcgrp,
    int timeout_ms);

/**
 * @brief Elect consumer group leader and send JoinGroup responses
 */
static void
rd_kafka_mock_cgrp_classic_elect_leader(rd_kafka_mock_cgrp_classic_t *mcgrp) {
        rd_kafka_mock_cgrp_classic_member_t *member;

        rd_assert(mcgrp->state == RD_KAFKA_MOCK_CGRP_STATE_JOINING);
        rd_assert(!TAILQ_EMPTY(&mcgrp->members));

        mcgrp->generation_id++;

        /* Elect a leader deterministically if the group.instance.id is
         * available, using the lexicographic order of group.instance.ids.
         * This is not how it's done on a real broker, which uses the first
         * member joined. But we use a determinstic method for better testing,
         * (in case we want to enforce a some consumer to be the group leader).
         * If group.instance.id is not specified for any consumer, we use the
         * first one joined, similar to the real broker. */
        mcgrp->leader = NULL;
        TAILQ_FOREACH(member, &mcgrp->members, link) {
                if (!mcgrp->leader)
                        mcgrp->leader = member;
                else if (mcgrp->leader->group_instance_id &&
                         member->group_instance_id &&
                         (rd_strcmp(mcgrp->leader->group_instance_id,
                                    member->group_instance_id) > 0))
                        mcgrp->leader = member;
        }

        rd_kafka_dbg(
            mcgrp->cluster->rk, MOCK, "MOCK",
            "Consumer group %s with %d member(s) is rebalancing: "
            "elected leader is %s (group.instance.id = %s), generation id %d",
            mcgrp->id, mcgrp->member_cnt, mcgrp->leader->id,
            mcgrp->leader->group_instance_id, mcgrp->generation_id);

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
                rd_kafka_mock_cgrp_classic_member_t *member2;
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
                rd_kafka_mock_cgrp_classic_member_active(mcgrp, member);

                rd_kafka_mock_connection_set_blocking(mconn, rd_false);
                rd_kafka_mock_connection_send_response(mconn, resp);
        }

        mcgrp->last_member_cnt = mcgrp->member_cnt;

        rd_kafka_mock_cgrp_classic_set_state(mcgrp,
                                             RD_KAFKA_MOCK_CGRP_STATE_SYNCING,
                                             "leader elected, waiting for all "
                                             "members to sync");

        rd_kafka_mock_cgrp_classic_rebalance_timer_restart(
            mcgrp, mcgrp->session_timeout_ms);
}


/**
 * @brief Trigger group rebalance.
 */
static void
rd_kafka_mock_cgrp_classic_rebalance(rd_kafka_mock_cgrp_classic_t *mcgrp,
                                     const char *reason) {
        int timeout_ms;

        if (mcgrp->state == RD_KAFKA_MOCK_CGRP_STATE_JOINING)
                return; /* Do nothing, group is already rebalancing. */
        else if (mcgrp->state == RD_KAFKA_MOCK_CGRP_STATE_EMPTY)
                /* First join, low timeout.
                 * Same as group.initial.rebalance.delay.ms
                 * on the broker. */
                timeout_ms =
                    mcgrp->cluster->defaults.group_initial_rebalance_delay_ms;
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
                rd_kafka_mock_cgrp_classic_sync_done(
                    mcgrp, RD_KAFKA_RESP_ERR_REBALANCE_IN_PROGRESS);

        rd_kafka_mock_cgrp_classic_set_state(
            mcgrp, RD_KAFKA_MOCK_CGRP_STATE_JOINING, reason);
        rd_kafka_mock_cgrp_classic_rebalance_timer_restart(mcgrp, timeout_ms);
}

/**
 * @brief Consumer group state machine triggered by timer events.
 */
static void
rd_kafka_mock_cgrp_classic_fsm_timeout(rd_kafka_mock_cgrp_classic_t *mcgrp) {
        rd_kafka_dbg(mcgrp->cluster->rk, MOCK, "MOCK",
                     "Mock consumer group %s FSM timeout in state %s",
                     mcgrp->id,
                     rd_kafka_mock_cgrp_classic_state_names[mcgrp->state]);

        switch (mcgrp->state) {
        case RD_KAFKA_MOCK_CGRP_STATE_EMPTY:
                /* No members, do nothing */
                break;
        case RD_KAFKA_MOCK_CGRP_STATE_JOINING:
                /* Timed out waiting for more members, elect a leader */
                if (mcgrp->member_cnt > 0)
                        rd_kafka_mock_cgrp_classic_elect_leader(mcgrp);
                else
                        rd_kafka_mock_cgrp_classic_set_state(
                            mcgrp, RD_KAFKA_MOCK_CGRP_STATE_EMPTY,
                            "no members joined");
                break;

        case RD_KAFKA_MOCK_CGRP_STATE_SYNCING:
                /* Timed out waiting for all members to sync */

                /* Send error response to all waiting members */
                rd_kafka_mock_cgrp_classic_sync_done(
                    mcgrp, RD_KAFKA_RESP_ERR_REBALANCE_IN_PROGRESS /* FIXME */);

                rd_kafka_mock_cgrp_classic_set_state(
                    mcgrp, RD_KAFKA_MOCK_CGRP_STATE_REBALANCING,
                    "timed out waiting for all members to synchronize");
                break;

        case RD_KAFKA_MOCK_CGRP_STATE_REBALANCING:
                /* Timed out waiting for all members to Leave or re-Join */
                rd_kafka_mock_cgrp_classic_set_state(
                    mcgrp, RD_KAFKA_MOCK_CGRP_STATE_JOINING,
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
        rd_kafka_mock_cgrp_classic_t *mcgrp = arg;

        rd_kafka_mock_cgrp_classic_fsm_timeout(mcgrp);
}


/**
 * @brief Restart the rebalance timer, postponing leader election.
 */
static void rd_kafka_mock_cgrp_classic_rebalance_timer_restart(
    rd_kafka_mock_cgrp_classic_t *mcgrp,
    int timeout_ms) {
        rd_kafka_timer_start_oneshot(
            &mcgrp->cluster->timers, &mcgrp->rebalance_tmr, rd_true,
            timeout_ms * 1000, rd_kafka_mcgrp_rebalance_timer_cb, mcgrp);
}


static void rd_kafka_mock_cgrp_classic_member_destroy(
    rd_kafka_mock_cgrp_classic_t *mcgrp,
    rd_kafka_mock_cgrp_classic_member_t *member) {
        rd_assert(mcgrp->member_cnt > 0);
        TAILQ_REMOVE(&mcgrp->members, member, link);
        mcgrp->member_cnt--;

        rd_free(member->id);

        if (member->resp)
                rd_kafka_buf_destroy(member->resp);

        if (member->group_instance_id)
                rd_free(member->group_instance_id);

        rd_kafka_mock_cgrp_classic_member_assignment_set(mcgrp, member, NULL);

        rd_kafka_mock_cgrp_classic_protos_destroy(member->protos,
                                                  member->proto_cnt);

        rd_free(member);
}


/**
 * @brief Find member in group.
 */
rd_kafka_mock_cgrp_classic_member_t *rd_kafka_mock_cgrp_classic_member_find(
    const rd_kafka_mock_cgrp_classic_t *mcgrp,
    const rd_kafkap_str_t *MemberId) {
        const rd_kafka_mock_cgrp_classic_member_t *member;
        TAILQ_FOREACH(member, &mcgrp->members, link) {
                if (!rd_kafkap_str_cmp_str(MemberId, member->id))
                        return (rd_kafka_mock_cgrp_classic_member_t *)member;
        }

        return NULL;
}


/**
 * @brief Update or add member to consumer group
 */
rd_kafka_resp_err_t rd_kafka_mock_cgrp_classic_member_add(
    rd_kafka_mock_cgrp_classic_t *mcgrp,
    rd_kafka_mock_connection_t *mconn,
    rd_kafka_buf_t *resp,
    const rd_kafkap_str_t *MemberId,
    const rd_kafkap_str_t *ProtocolType,
    const rd_kafkap_str_t *GroupInstanceId,
    rd_kafka_mock_cgrp_classic_proto_t *protos,
    int proto_cnt,
    int session_timeout_ms) {
        rd_kafka_mock_cgrp_classic_member_t *member;
        rd_kafka_resp_err_t err;

        err = rd_kafka_mock_cgrp_classic_check_state(mcgrp, NULL, resp, -1);
        if (err)
                return err;

        /* Find member */
        member = rd_kafka_mock_cgrp_classic_member_find(mcgrp, MemberId);
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

                if (RD_KAFKAP_STR_LEN(GroupInstanceId))
                        member->group_instance_id =
                            RD_KAFKAP_STR_DUP(GroupInstanceId);

                TAILQ_INSERT_TAIL(&mcgrp->members, member, link);
                mcgrp->member_cnt++;
        }

        if (mcgrp->state != RD_KAFKA_MOCK_CGRP_STATE_JOINING)
                rd_kafka_mock_cgrp_classic_rebalance(mcgrp, "member join");

        mcgrp->session_timeout_ms = session_timeout_ms;

        if (member->protos)
                rd_kafka_mock_cgrp_classic_protos_destroy(member->protos,
                                                          member->proto_cnt);
        member->protos    = protos;
        member->proto_cnt = proto_cnt;

        rd_assert(!member->resp);
        member->resp = resp;
        member->conn = mconn;
        rd_kafka_mock_cgrp_classic_member_active(mcgrp, member);

        return RD_KAFKA_RESP_ERR_NO_ERROR;
}

/**
 * @brief Check if any members have exceeded the session timeout.
 */
static void rd_kafka_mock_cgrp_classic_session_tmr_cb(rd_kafka_timers_t *rkts,
                                                      void *arg) {
        rd_kafka_mock_cgrp_classic_t *mcgrp = arg;
        rd_kafka_mock_cgrp_classic_member_t *member, *tmp;
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

                rd_kafka_mock_cgrp_classic_member_destroy(mcgrp, member);
                timeout_cnt++;
        }

        if (timeout_cnt)
                rd_kafka_mock_cgrp_classic_rebalance(mcgrp, "member timeout");
}


void rd_kafka_mock_cgrp_classic_destroy(rd_kafka_mock_cgrp_classic_t *mcgrp) {
        rd_kafka_mock_cgrp_classic_member_t *member;

        TAILQ_REMOVE(&mcgrp->cluster->cgrps_classic, mcgrp, link);

        rd_kafka_timer_stop(&mcgrp->cluster->timers, &mcgrp->rebalance_tmr,
                            rd_true);
        rd_kafka_timer_stop(&mcgrp->cluster->timers, &mcgrp->session_tmr,
                            rd_true);
        rd_free(mcgrp->id);
        rd_free(mcgrp->protocol_type);
        if (mcgrp->protocol_name)
                rd_free(mcgrp->protocol_name);
        while ((member = TAILQ_FIRST(&mcgrp->members)))
                rd_kafka_mock_cgrp_classic_member_destroy(mcgrp, member);
        rd_free(mcgrp);
}


rd_kafka_mock_cgrp_classic_t *
rd_kafka_mock_cgrp_classic_find(rd_kafka_mock_cluster_t *mcluster,
                                const rd_kafkap_str_t *GroupId) {
        rd_kafka_mock_cgrp_classic_t *mcgrp;
        TAILQ_FOREACH(mcgrp, &mcluster->cgrps_classic, link) {
                if (!rd_kafkap_str_cmp_str(GroupId, mcgrp->id))
                        return mcgrp;
        }

        return NULL;
}


/**
 * @brief Find or create a classic consumer group
 */
rd_kafka_mock_cgrp_classic_t *
rd_kafka_mock_cgrp_classic_get(rd_kafka_mock_cluster_t *mcluster,
                               const rd_kafkap_str_t *GroupId,
                               const rd_kafkap_str_t *ProtocolType) {
        rd_kafka_mock_cgrp_classic_t *mcgrp;

        mcgrp = rd_kafka_mock_cgrp_classic_find(mcluster, GroupId);
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
                             rd_kafka_mock_cgrp_classic_session_tmr_cb, mcgrp);

        TAILQ_INSERT_TAIL(&mcluster->cgrps_classic, mcgrp, link);

        return mcgrp;
}


/**
 * @brief A client connection closed, check if any classic cgrp has any state
 *        for this connection that needs to be cleared.
 */
void rd_kafka_mock_cgrps_classic_connection_closed(
    rd_kafka_mock_cluster_t *mcluster,
    rd_kafka_mock_connection_t *mconn) {
        rd_kafka_mock_cgrp_classic_t *mcgrp;

        TAILQ_FOREACH(mcgrp, &mcluster->cgrps_classic, link) {
                rd_kafka_mock_cgrp_classic_member_t *member, *tmp;
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

/**
 * @struct Target assignment for a consumer group.
 *         `member_ids` and `assignment` are in the same order
 *         and have the same count.
 */
typedef struct rd_kafka_mock_cgrp_consumer_target_assignment_s {
        rd_list_t *member_ids; /**< Member id list (char *). */
        rd_list_t *assignment; /**< Assingment list
                                  (rd_kafka_topic_partition_list_t *). */
} rd_kafka_mock_cgrp_consumer_target_assignment_t;

static rd_kafka_mock_cgrp_consumer_target_assignment_t *
rd_kafka_mock_cgrp_consumer_target_assignment_new0(rd_list_t *member_ids,
                                                   rd_list_t *assignment) {
        rd_assert(member_ids->rl_cnt == assignment->rl_cnt);
        rd_kafka_mock_cgrp_consumer_target_assignment_t *target_assignment =
            rd_calloc(1, sizeof(*target_assignment));
        target_assignment->member_ids =
            rd_list_copy(member_ids, rd_list_string_copy, NULL);
        target_assignment->assignment = rd_list_copy(
            assignment, rd_kafka_topic_partition_list_copy_opaque, NULL);
        return target_assignment;
}

rd_kafka_mock_cgrp_consumer_target_assignment_t *
rd_kafka_mock_cgrp_consumer_target_assignment_new(
    char **member_ids,
    int member_cnt,
    rd_kafka_topic_partition_list_t **assignment) {
        int i;
        rd_list_t *member_id_list, *assignment_list;
        rd_kafka_mock_cgrp_consumer_target_assignment_t *ret;

        member_id_list = rd_list_new(member_cnt, rd_free);
        assignment_list =
            rd_list_new(member_cnt, rd_kafka_topic_partition_list_destroy_free);
        for (i = 0; i < member_cnt; i++) {
                rd_list_add(member_id_list, rd_strdup(member_ids[i]));
                rd_list_add(assignment_list,
                            rd_kafka_topic_partition_list_copy(assignment[i]));
        }

        ret = rd_kafka_mock_cgrp_consumer_target_assignment_new0(
            member_id_list, assignment_list);
        rd_list_destroy(member_id_list);
        rd_list_destroy(assignment_list);
        return ret;
}

void rd_kafka_mock_cgrp_consumer_target_assignment_destroy(
    rd_kafka_mock_cgrp_consumer_target_assignment_t *target_assignment) {
        rd_list_destroy(target_assignment->member_ids);
        rd_list_destroy(target_assignment->assignment);
        rd_free(target_assignment);
}

/**
 * @brief Sets next target assignment and member epoch for \p member
 *        to a copy of partition list \p rktparlist,
 *        filling its topic ids if not provided, using \p cgrp cluster topics.
 *
 * @param mcgrp The consumer group containing the member.
 * @param member A consumer group member.
 * @param target_member_epoch New member epoch.
 * @param rktparlist Next target assignment.
 *
 * @locks mcluster->lock MUST be held.
 */
static void rd_kafka_mock_cgrp_consumer_member_target_assignment_set(
    rd_kafka_mock_cgrp_consumer_t *mcgrp,
    rd_kafka_mock_cgrp_consumer_member_t *member,
    int target_member_epoch,
    const rd_kafka_topic_partition_list_t *rktparlist) {
        rd_kafka_topic_partition_t *rktpar;
        if (member->target_assignment) {
                rd_kafka_topic_partition_list_destroy(
                    member->target_assignment);
        }
        member->target_member_epoch = target_member_epoch;
        member->target_assignment =
            rd_kafka_topic_partition_list_copy(rktparlist);

        /* If not present, fill topic ids using names */
        RD_KAFKA_TPLIST_FOREACH(rktpar, member->target_assignment) {
                rd_kafka_Uuid_t topic_id =
                    rd_kafka_topic_partition_get_topic_id(rktpar);
                if (!rd_kafka_Uuid_cmp(topic_id, RD_KAFKA_UUID_ZERO)) {
                        rd_kafka_mock_topic_t *mtopic =
                            rd_kafka_mock_topic_find(mcgrp->cluster,
                                                     rktpar->topic);
                        if (mtopic)
                                rd_kafka_topic_partition_set_topic_id(
                                    rktpar, mtopic->id);
                }
        }
}

/**
 * @brief Sets next target assignment for group \p mcgrp
 *        to a copy of \p target_assignment partition lists.
 *
 * @param mcgrp The consumer group.
 * @param target_assignment Target assignment for all members.
 *
 * @locks mcluster->lock MUST be held.
 */
static void rd_kafka_mock_cgrp_consumer_target_assignment_set(
    rd_kafka_mock_cgrp_consumer_t *mcgrp,
    rd_kafka_mock_cgrp_consumer_target_assignment_t *target_assignment) {
        int i = 0;
        int32_t new_target_member_epoch;
        const char *member_id;
        rd_kafka_mock_cgrp_consumer_member_t *member;

        mcgrp->group_epoch++;
        new_target_member_epoch = mcgrp->group_epoch;
        RD_LIST_FOREACH(member_id, target_assignment->member_ids, i) {
                rd_kafkap_str_t *member_id_str =
                    rd_kafkap_str_new(member_id, strlen(member_id));
                rd_kafka_topic_partition_list_t *member_assignment =
                    rd_list_elem(target_assignment->assignment, i);
                member = rd_kafka_mock_cgrp_consumer_member_find(mcgrp,
                                                                 member_id_str);
                rd_kafkap_str_destroy(member_id_str);

                if (!member)
                        continue;

                rd_kafka_mock_cgrp_consumer_member_target_assignment_set(
                    mcgrp, member, new_target_member_epoch, member_assignment);
        }
}

typedef RD_MAP_TYPE(const char *, rd_list_t *) map_str_list;
typedef RD_MAP_TYPE(const char *, int *) map_str_int;

/**
 * @brief Calculate a simple range target assignment for the consumer group \p
 * mcgrp. This isn't replicating any given broker assignor but is used
 * when the test doesn't need a specific type of assignment.
 *
 * If the test needs it, instead of replicating same conditions with all the
 * members, one can mock the assignment directly with
 * `rd_kafka_mock_cgrp_consumer_target_assignment`.
 */
static rd_kafka_mock_cgrp_consumer_target_assignment_t *
rd_kafka_mock_cgrp_consumer_target_assignment_calculate_range(
    const rd_kafka_mock_cgrp_consumer_t *mcgrp) {
        int i, *i_pointer;
        const char *topic;
        rd_list_t *members;
        rd_kafka_mock_cgrp_consumer_member_t *member;
        rd_kafka_mock_cluster_t *mcluster = mcgrp->cluster;
        /* List of member ids (char *) */
        rd_list_t *member_ids = rd_list_new(mcgrp->member_cnt, rd_free);
        /* List of member assignment (rd_kafka_topic_partition_list_t *) */
        rd_list_t *assignment = rd_list_new(
            mcgrp->member_cnt, rd_kafka_topic_partition_list_destroy_free);
        /* Map from topic name to list of members */
        map_str_list topic_members =
            RD_MAP_INITIALIZER(mcgrp->member_cnt, rd_map_str_cmp,
                               rd_map_str_hash, NULL, rd_list_destroy_free);
        /* Map from member id to index in the members and assignment lists. */
        map_str_int member_idx = RD_MAP_INITIALIZER(
            mcgrp->member_cnt, rd_map_str_cmp, rd_map_str_hash, NULL, rd_free);

        i = 0;

        /* First create a map with topics associated to the list of members
         * and save the member idx in the `member_idx` map. */
        TAILQ_FOREACH(member, &mcgrp->members, link) {
                int j;
                rd_list_add(member_ids, rd_strdup(member->id));
                rd_list_add(assignment, rd_kafka_topic_partition_list_new(0));

                RD_LIST_FOREACH(topic, member->subscribed_topics, j) {
                        if (!RD_MAP_GET(&topic_members, topic)) {
                                members = rd_list_new(0, NULL);
                                RD_MAP_SET(&topic_members, topic, members);
                        } else
                                members = RD_MAP_GET(&topic_members, topic);
                        rd_list_add(members, member);
                }
                i_pointer  = rd_calloc(1, sizeof(*i_pointer));
                *i_pointer = i;
                RD_MAP_SET(&member_idx, member->id, i_pointer);
                i++;
        }

        /* For each topic to a range assignment and add the
         * corresponding partitions to the assignment for that member.
         * Finds the list index using the `member_idx` map. */
        RD_MAP_FOREACH(topic, members, &topic_members) {
                rd_kafka_Uuid_t topic_id;
                rd_kafka_topic_partition_list_t *member_assignment;
                int members_cnt = rd_list_cnt(members);
                int common, one_more, assigned = 0;
                rd_kafkap_str_t Topic = {.str = topic, .len = strlen(topic)};
                rd_kafka_mock_topic_t *mock_topic =
                    rd_kafka_mock_topic_find_by_kstr(mcluster, &Topic);
                if (!mock_topic)
                        continue;

                topic_id = mock_topic->id;

                /* Assign one partition more
                 * to the first mock_topic->partition_cnt % members_cnt
                 * members. */
                common   = mock_topic->partition_cnt / members_cnt;
                one_more = mock_topic->partition_cnt % members_cnt;

                RD_LIST_FOREACH(member, members, i) {
                        int j, num_partitions = common;
                        int idx = *RD_MAP_GET(&member_idx, member->id);
                        member_assignment = rd_list_elem(assignment, idx);
                        if (idx < one_more)
                                num_partitions++;
                        for (j = 0; j < num_partitions; j++) {
                                rd_kafka_topic_partition_t *rktpar =
                                    rd_kafka_topic_partition_list_add(
                                        member_assignment, topic, assigned + j);
                                rd_kafka_topic_partition_set_topic_id(rktpar,
                                                                      topic_id);
                        }
                        assigned += num_partitions;
                }
        }

        rd_kafka_mock_cgrp_consumer_target_assignment_t *ret =
            rd_kafka_mock_cgrp_consumer_target_assignment_new0(member_ids,
                                                               assignment);

        RD_MAP_DESTROY(&topic_members);
        RD_MAP_DESTROY(&member_idx);

        rd_list_destroy(member_ids);
        rd_list_destroy(assignment);

        return ret;
}

/**
 * @brief Recalculate and set a target assignment for \p mcgrp
 *        only if `mcgrp->manual_assignment` isn't set.
 *
 * @locks mcluster->lock MUST be held.
 */
static void rd_kafka_mock_cgrp_consumer_target_assignment_recalculate(
    rd_kafka_mock_cgrp_consumer_t *mcgrp) {
        if (mcgrp->manual_assignment)
                return;

        rd_kafka_mock_cgrp_consumer_target_assignment_t *target_assignment =
            rd_kafka_mock_cgrp_consumer_target_assignment_calculate_range(
                mcgrp);
        rd_kafka_mock_cgrp_consumer_target_assignment_set(mcgrp,
                                                          target_assignment);
        rd_kafka_mock_cgrp_consumer_target_assignment_destroy(
            target_assignment);
}

/**
 * @brief Set manual target assignment \p target_assignment
 *        to the consumer group \p mcgrp .
 *
 * @param mcgrp Consumer group
 * @param target_assignment Target assignment to set.
 *                          Pass NULL to return to automatic assignment.
 *
 * @locks mcluster->lock MUST be held.
 */
static void rd_kafka_mock_cgrp_consumer_target_assignment_set_manual(
    rd_kafka_mock_cgrp_consumer_t *mcgrp,
    rd_kafka_mock_cgrp_consumer_target_assignment_t *target_assignment) {
        if (!target_assignment) {
                mcgrp->manual_assignment = rd_false;
                rd_kafka_mock_cgrp_consumer_target_assignment_recalculate(
                    mcgrp);
                return;
        }

        mcgrp->manual_assignment = rd_true;

        rd_kafka_mock_cgrp_consumer_target_assignment_set(mcgrp,
                                                          target_assignment);
}

/**
 * @brief Sets \p member current assignment to a copy of
 *        \p current_assignment.
 *
 * @param member A consumer group member.
 * @param current_assignment Current assignment to set.
 *
 * @locks mcluster->lock MUST be held.
 */
static void rd_kafka_mock_cgrp_consumer_member_current_assignment_set(
    rd_kafka_mock_cgrp_consumer_member_t *member,
    const rd_kafka_topic_partition_list_t *current_assignment) {
        if (member->current_assignment) {
                rd_kafka_topic_partition_list_destroy(
                    member->current_assignment);
        }

        member->current_assignment =
            current_assignment
                ? rd_kafka_topic_partition_list_copy(current_assignment)
                : NULL;
}

/**
 * @brief Sets \p member returned assignment to a
 *        copy of \p returned_assignment.
 *
 * @param member A consumer group member.
 * @param returned_assignment Returned assignment to set.
 *
 * @locks mcluster->lock MUST be held.
 */
static void rd_kafka_mock_cgrp_consumer_member_returned_assignment_set(
    rd_kafka_mock_cgrp_consumer_member_t *member,
    const rd_kafka_topic_partition_list_t *returned_assignment) {
        if (member->returned_assignment) {
                rd_kafka_topic_partition_list_destroy(
                    member->returned_assignment);
        }
        member->returned_assignment =
            returned_assignment
                ? rd_kafka_topic_partition_list_copy(returned_assignment)
                : NULL;
}

/**
 * @brief Returns a copy of \p member target assignment containing only
 * partitions that can be assignment, whose topic id is non-zero.
 *
 * @param member The group member.
 *
 * @remark The returned pointer ownership is transferred to the caller.
 *
 * @locks mcluster->lock MUST be held.
 */
static rd_kafka_topic_partition_list_t *
rd_kafka_mock_cgrp_consumer_member_target_assignment_assignable(
    rd_kafka_mock_cgrp_consumer_member_t *member) {
        rd_kafka_topic_partition_list_t *assignment = member->target_assignment;
        rd_kafka_topic_partition_t *rktpar;
        rd_kafka_topic_partition_list_t *ret =
            rd_kafka_topic_partition_list_new(assignment->cnt);

        RD_KAFKA_TPLIST_FOREACH(rktpar, assignment) {
                rd_kafka_Uuid_t topic_id =
                    rd_kafka_topic_partition_get_topic_id(rktpar);
                if (rd_kafka_Uuid_cmp(topic_id, RD_KAFKA_UUID_ZERO)) {
                        rd_kafka_topic_partition_list_add_copy(ret, rktpar);
                }
        }

        return ret;
}

/**
 * Returns true iff \p new_assignment doesn't have any intersection with any
 * other member current assignment.
 *
 * If there's an intersection, it means we cannot bump the epoch at the moment,
 * because some of these partitions are held by a different member. They have
 * to be revoked from that member before it's possible to increase the epoch
 * and assign additional partitions to this member.
 */
rd_bool_t rd_kafka_mock_cgrp_consumer_member_next_assignment_can_bump_epoch(
    rd_kafka_mock_cgrp_consumer_member_t *member,
    rd_kafka_topic_partition_list_t *new_assignment) {
        rd_kafka_topic_partition_list_t *double_assignment,
            *assigned_partitions = rd_kafka_topic_partition_list_new(0);
        rd_kafka_mock_cgrp_consumer_member_t *other_member;
        rd_kafka_mock_cgrp_consumer_t *mcgrp = member->mcgrp;
        rd_bool_t ret;

        TAILQ_FOREACH(other_member, &mcgrp->members, link) {
                int other_current_assignment_cnt  = 0,
                    other_returned_assignment_cnt = 0;
                if (member == other_member)
                        continue;
                if (other_member->current_assignment)
                        other_current_assignment_cnt =
                            other_member->current_assignment->cnt;
                if (other_member->returned_assignment)
                        other_returned_assignment_cnt =
                            other_member->returned_assignment->cnt;

                if (other_current_assignment_cnt > 0 &&
                    other_current_assignment_cnt >
                        other_returned_assignment_cnt) {
                        /* This is the case where we're revoking
                         * some partitions.
                         * returned_assignment < current_assignment. */
                        rd_kafka_topic_partition_list_add_list(
                            assigned_partitions,
                            other_member->current_assignment);
                } else if (other_returned_assignment_cnt > 0) {
                        /* This is the case where we're assigning
                         * some partitions.
                         * returned_assignment >= current_assignment. */
                        rd_kafka_topic_partition_list_add_list(
                            assigned_partitions,
                            other_member->returned_assignment);
                }
        }
        double_assignment = rd_kafka_topic_partition_list_intersection_by_id(
            new_assignment, assigned_partitions);
        ret = double_assignment->cnt == 0;

        rd_kafka_topic_partition_list_destroy(assigned_partitions);
        rd_kafka_topic_partition_list_destroy(double_assignment);
        return ret;
}

/**
 * @brief Calculates if \p member,
 *        needs a revocation, that is if its current assignment
 *        isn't a subset of its target assignment.
 *        In case it needs a revocation, it returns
 *        the intersection between the two assignments,
 *        that is the remaining partitions after revocation
 *        of those not included in target assignment.
 *
 * @param member The group member.
 *
 * @return The remaining set of partitions, or NULL in case no revocation
 *         is needed.
 *
 * @remark The returned pointer ownership is transferred to the caller.
 *
 * @locks mcluster->lock MUST be held.
 */
static rd_kafka_topic_partition_list_t *
rd_kafka_mock_cgrp_consumer_member_needs_revocation(
    rd_kafka_mock_cgrp_consumer_member_t *member) {
        rd_kafka_topic_partition_list_t *intersection;
        rd_bool_t needs_revocation;

        if (member->current_assignment)
                /* If we have a current assignment we
                 * calculate the intersection with
                 * target assignment. */
                intersection = rd_kafka_topic_partition_list_intersection_by_id(
                    member->current_assignment, member->target_assignment);
        else
                /* Otherwise intersection is empty. */
                intersection = rd_kafka_topic_partition_list_new(0);

        needs_revocation = member->current_assignment &&
                           intersection->cnt < member->current_assignment->cnt;
        if (needs_revocation) {
                return intersection;
        }

        rd_kafka_topic_partition_list_destroy(intersection);
        return NULL;
}

/**
 * @brief Calculates if \p member,
 *        can receive new partitions, given revocation is completed.
 *        In case new partitions aren't held by other members it
 *        returns the assignable target assignment and bumps current
 *        member epoch, otherwise it returns NULL and
 *        doesn't change current member epoch.
 *
 * @param member The group member.
 *
 * @return The assignable set of partitions, or NULL in case new partitions
 *         cannot be assigned yet.
 *
 * @remark The returned pointer ownership is transferred to the caller.
 *
 * @locks mcluster->lock MUST be held.
 */
static rd_kafka_topic_partition_list_t *
rd_kafka_mock_cgrp_consumer_member_needs_assignment(
    rd_kafka_mock_cgrp_consumer_member_t *member) {
        rd_kafka_topic_partition_list_t *returned_assignment =
            rd_kafka_mock_cgrp_consumer_member_target_assignment_assignable(
                member);

        if (!rd_kafka_mock_cgrp_consumer_member_next_assignment_can_bump_epoch(
                member, returned_assignment)) {
                /* We can't bump the epoch still,
                 * there are some partitions held by other members.
                 * We have to return NULL. */
                rd_kafka_topic_partition_list_destroy(returned_assignment);
                return NULL;
        }

        /* No partitions to remove, return
         * target assignment and reconcile the
         * epochs */
        member->current_member_epoch = member->target_member_epoch;
        return returned_assignment;
}

/**
 * @brief Calculates next assignment and member epoch for a \p member,
 *        given \p current_assignment.
 *
 * @param member The group member.
 * @param current_assignment The assignment sent by the member, or NULL if it
 *                           didn't change. Must be NULL if *member_epoch is 0.
 * @param member_epoch Pointer to client reported member epoch. Can be updated.
 *
 * @return The new assignment to return to the member.
 *
 * @remark The returned pointer ownership is transferred to the caller.
 *
 * @locks mcluster->lock MUST be held.
 */
rd_kafka_topic_partition_list_t *
rd_kafka_mock_cgrp_consumer_member_next_assignment(
    rd_kafka_mock_cgrp_consumer_member_t *member,
    rd_kafka_topic_partition_list_t *current_assignment,
    int *member_epoch) {
        rd_kafka_topic_partition_list_t *assignment_to_return = NULL;

        if (current_assignment) {
                /* Update current assignment to reflect what is provided
                 * by the client. */
                rd_kafka_mock_cgrp_consumer_member_current_assignment_set(
                    member, current_assignment);
        }

        if (*member_epoch > 0 &&
            member->current_member_epoch != *member_epoch) {
                /* Member epoch is different from the one we expect,
                 * that means we have to fence the member. */
                *member_epoch = -1; /* FENCED_MEMBER_EPOCH */
                return NULL;
        }

        if (member->target_assignment) {
                /* We have a target assignment,
                 * let's check if we can assign it. */

                if (*member_epoch != member->current_member_epoch ||
                    member->current_member_epoch !=
                        member->target_member_epoch) {
                        /* Epochs are different, that means we have to bump the
                         * epoch immediately or do some revocations
                         * before that. */

                        assignment_to_return =
                            rd_kafka_mock_cgrp_consumer_member_needs_revocation(
                                member);
                        if (!assignment_to_return) {
                                /* After revocation we only have to
                                 * add new partitions.
                                 * In case these new partitions are held
                                 * by other members we still cannot do it. */
                                assignment_to_return =
                                    rd_kafka_mock_cgrp_consumer_member_needs_assignment(
                                        member);
                        }
                } else if (!member->returned_assignment) {
                        /* If all the epochs are the same, the only case
                         * where we have to return the assignment is
                         * after a disconnection, when returned_assignment has
                         * been reset to NULL. */
                        assignment_to_return =
                            rd_kafka_mock_cgrp_consumer_member_target_assignment_assignable(
                                member);
                }
        }

        *member_epoch = member->current_member_epoch;
        if (assignment_to_return) {
                /* Compare assignment_to_return with last returned_assignment.
                 * If equal, return NULL, otherwise return assignment_to_return
                 * and update last returned_assignment. */
                rd_bool_t same_returned_assignment =
                    member->returned_assignment &&
                    !rd_kafka_topic_partition_list_cmp(
                        member->returned_assignment, assignment_to_return,
                        rd_kafka_topic_partition_by_id_cmp);

                if (same_returned_assignment) {
                        /* Returned assignment is the same as previous
                         * one, we return NULL instead to show no change. */
                        rd_kafka_topic_partition_list_destroy(
                            assignment_to_return);
                        assignment_to_return = NULL;
                } else {
                        /* We store returned assignment
                         * for later comparison. */
                        rd_kafka_mock_cgrp_consumer_member_returned_assignment_set(
                            member, assignment_to_return);
                }
        }
        return assignment_to_return;
}

/**
 * @brief Mark member as active (restart session timer).
 *
 * @param mcgrp Member's consumer group.
 * @param member Member to set as active.
 *
 * @locks mcluster->lock MUST be held.
 */
void rd_kafka_mock_cgrp_consumer_member_active(
    rd_kafka_mock_cgrp_consumer_t *mcgrp,
    rd_kafka_mock_cgrp_consumer_member_t *member) {
        rd_kafka_dbg(mcgrp->cluster->rk, MOCK, "MOCK",
                     "Marking mock consumer group member %s as active",
                     member->id);
        member->ts_last_activity = rd_clock();
}

/**
 * @brief Finds a member in consumer group \p mcgrp by \p MemberId.
 *
 * @param mcgrp Consumer group to search.
 * @param MemberId Member id to look for.
 * @return Found member or NULL.
 *
 * @locks mcluster->lock MUST be held.
 */
rd_kafka_mock_cgrp_consumer_member_t *rd_kafka_mock_cgrp_consumer_member_find(
    const rd_kafka_mock_cgrp_consumer_t *mcgrp,
    const rd_kafkap_str_t *MemberId) {
        const rd_kafka_mock_cgrp_consumer_member_t *member;
        TAILQ_FOREACH(member, &mcgrp->members, link) {
                if (!rd_kafkap_str_cmp_str(MemberId, member->id))
                        return (rd_kafka_mock_cgrp_consumer_member_t *)member;
        }

        return NULL;
}

/**
 * @brief Finds a member in consumer group \p mcgrp by \p InstanceId.
 *
 * @param mcgrp Consumer group to search.
 * @param InstanceId Instance id to look for.
 * @return Found member or NULL.
 *
 * @locks mcluster->lock MUST be held.
 */
rd_kafka_mock_cgrp_consumer_member_t *
rd_kafka_mock_cgrp_consumer_member_find_by_instance_id(
    const rd_kafka_mock_cgrp_consumer_t *mcgrp,
    const rd_kafkap_str_t *InstanceId) {
        if (RD_KAFKAP_STR_IS_NULL(InstanceId))
                return NULL;

        const rd_kafka_mock_cgrp_consumer_member_t *member;
        TAILQ_FOREACH(member, &mcgrp->members, link) {
                if (!member->instance_id)
                        continue;

                if (!rd_kafkap_str_cmp_str(InstanceId, member->instance_id))
                        return (rd_kafka_mock_cgrp_consumer_member_t *)member;
        }

        return NULL;
}

static void validate_subscription(const rd_kafkap_str_t *SubscribedTopicNames,
                                  int32_t SubscribedTopicNamesCnt,
                                  const rd_kafkap_str_t *SubscribedTopicRegex) {
        /* Either they are both NULL
         * or both non-NULL. */
        rd_assert((SubscribedTopicNames == NULL) ==
                  RD_KAFKAP_STR_IS_NULL(SubscribedTopicRegex));
        /* If they're not NULL at least one should be non-empty */
        rd_assert(SubscribedTopicNames == NULL || SubscribedTopicNamesCnt > 0 ||
                  RD_KAFKAP_STR_LEN(SubscribedTopicRegex) > 0);
}

/**
 * @brief Set the subscribed topics for the member \p member based on \p
 * SubscribedTopicNames and \p SubscribedTopicRegex. Deduplicates the list after
 * sorting it.
 * @return `rd_true` if the subscription was changed, that happens
 *         if it's set and different from previous one.
 *
 * @locks mcluster->lock MUST be held.
 */
static rd_bool_t rd_kafka_mock_cgrp_consumer_member_subscribed_topic_names_set(
    rd_kafka_mock_cgrp_consumer_member_t *member,
    rd_kafkap_str_t *SubscribedTopicNames,
    int32_t SubscribedTopicNamesCnt,
    const rd_kafkap_str_t *SubscribedTopicRegex) {
        rd_bool_t changed = rd_false;
        rd_list_t *new_subscription;
        int32_t i;

        validate_subscription(SubscribedTopicNames, SubscribedTopicNamesCnt,
                              SubscribedTopicRegex);

        if (!SubscribedTopicNames &&
            RD_KAFKAP_STR_IS_NULL(SubscribedTopicRegex) &&
            !member->subscribed_topic_regex) {
                /* When client is sending NULL for SubscribedTopicNames and
                 * SubscribedTopicRegex, its subscription didn't change. If we
                 * already had a regex, we need to compute the regex again. */
                return changed;
        }

        if (SubscribedTopicNames) {
                RD_IF_FREE(member->subscribed_topic_names, rd_list_destroy);
                member->subscribed_topic_names =
                    rd_list_new(SubscribedTopicNamesCnt, rd_free);
                for (i = 0; i < SubscribedTopicNamesCnt; i++) {
                        rd_list_add(
                            member->subscribed_topic_names,
                            RD_KAFKAP_STR_DUP(&SubscribedTopicNames[i]));
                }
        }

        if (!RD_KAFKAP_STR_IS_NULL(SubscribedTopicRegex)) {
                RD_IF_FREE(member->subscribed_topic_regex, rd_free);
                member->subscribed_topic_regex =
                    RD_KAFKAP_STR_DUP(SubscribedTopicRegex);
        }

        new_subscription =
            rd_list_new(rd_list_cnt(member->subscribed_topic_names), rd_free);

        rd_list_copy_to(new_subscription, member->subscribed_topic_names,
                        rd_list_string_copy, NULL);

        if (member->subscribed_topic_regex[0]) {
                rd_kafka_mock_cluster_t *mcluster = member->mcgrp->cluster;
                rd_kafka_mock_topic_t *mtopic;
                char errstr[1];
                rd_regex_t *re = rd_regex_comp(member->subscribed_topic_regex,
                                               errstr, sizeof(errstr));

                TAILQ_FOREACH(mtopic, &mcluster->topics, link) {
                        if (rd_regex_exec(re, mtopic->name))
                                rd_list_add(new_subscription,
                                            rd_strdup(mtopic->name));
                }

                rd_regex_destroy(re);
        }

        rd_list_deduplicate(&new_subscription, rd_strcmp2);

        if (!member->subscribed_topics ||
            rd_list_cmp(new_subscription, member->subscribed_topics,
                        rd_list_cmp_str)) {
                if (member->subscribed_topics)
                        rd_list_destroy(member->subscribed_topics);
                member->subscribed_topics =
                    rd_list_copy(new_subscription, rd_list_string_copy, NULL);
                changed = rd_true;
        }
        rd_list_destroy(new_subscription);
        return changed;
}

static void rd_kafka_mock_cgrp_consumer_member_topic_id_set(
    rd_kafka_mock_cgrp_consumer_member_t *member,
    const rd_kafkap_str_t *MemberId) {
        /* KIP 1082: MemberId is generated by the client */
        rd_assert(RD_KAFKAP_STR_LEN(MemberId) > 0);
        RD_IF_FREE(member->id, rd_free);
        member->id = RD_KAFKAP_STR_DUP(MemberId);
}

/**
 * @brief Adds a member to consumer group \p mcgrp. If member with same
 *        \p MemberId is already present, only updates the connection and
 *        sets it as active.
 *
 * @param mcgrp Consumer group to add the member to.
 * @param conn Member connection.
 * @param MemberId Member id.
 * @param InstanceId Group instance id (optional).
 * @param session_timeout_ms Session timeout to use.
 * @param SubscribedTopicNames Array of subscribed topics.
 *                             Mandatory if the member is a new one.
 * @param SubscribedTopicNamesCnt Number of elements in \p SubscribedTopicNames.
 * @param SubscribedTopicRegex Subscribed topic regex.
 *
 * @return New or existing member, NULL if the member cannot be added.
 *
 * @locks mcluster->lock MUST be held.
 */
rd_kafka_mock_cgrp_consumer_member_t *rd_kafka_mock_cgrp_consumer_member_add(
    rd_kafka_mock_cgrp_consumer_t *mcgrp,
    struct rd_kafka_mock_connection_s *conn,
    const rd_kafkap_str_t *MemberId,
    const rd_kafkap_str_t *InstanceId,
    rd_kafkap_str_t *SubscribedTopicNames,
    int32_t SubscribedTopicNamesCnt,
    const rd_kafkap_str_t *SubscribedTopicRegex) {
        rd_kafka_mock_cgrp_consumer_member_t *member = NULL;
        rd_bool_t changed                            = rd_false;

        /* Find member */
        member = rd_kafka_mock_cgrp_consumer_member_find(mcgrp, MemberId);
        if (!member) {
                member = rd_kafka_mock_cgrp_consumer_member_find_by_instance_id(
                    mcgrp, InstanceId);

                if (member) {
                        if (!member->left_static_membership) {
                                /* Old member still active,
                                 * fence this one */
                                return NULL;
                        }

                        if (rd_kafkap_str_cmp_str(MemberId, member->id) != 0) {
                                /* Member is a new instance and is rejoining
                                 * with a new MemberId. */
                                rd_kafka_mock_cgrp_consumer_member_topic_id_set(
                                    member, MemberId);
                        }
                        member->left_static_membership = rd_false;
                }
        } else {
                member->left_static_membership = rd_false;
        }

        if (!member) {
                validate_subscription(SubscribedTopicNames,
                                      SubscribedTopicNamesCnt,
                                      SubscribedTopicRegex);

                /* In case of session timeout
                 * where the member isn't aware it's been fenced. */
                if (SubscribedTopicNames == NULL)
                        return NULL;

                /* Not found, add member */
                member        = rd_calloc(1, sizeof(*member));
                member->mcgrp = mcgrp;

                rd_kafka_mock_cgrp_consumer_member_topic_id_set(member,
                                                                MemberId);

                if (!RD_KAFKAP_STR_IS_NULL(InstanceId))
                        member->instance_id = RD_KAFKAP_STR_DUP(InstanceId);

                TAILQ_INSERT_TAIL(&mcgrp->members, member, link);
                mcgrp->member_cnt++;
                changed                     = rd_true;
                member->target_member_epoch = mcgrp->group_epoch;
        }

        changed |=
            rd_kafka_mock_cgrp_consumer_member_subscribed_topic_names_set(
                member, SubscribedTopicNames, SubscribedTopicNamesCnt,
                SubscribedTopicRegex);

        mcgrp->session_timeout_ms =
            mcgrp->cluster->defaults.group_consumer_session_timeout_ms;
        mcgrp->heartbeat_interval_ms =
            mcgrp->cluster->defaults.group_consumer_heartbeat_interval_ms;

        member->conn = conn;

        rd_kafka_mock_cgrp_consumer_member_active(mcgrp, member);

        if (changed)
                rd_kafka_mock_cgrp_consumer_target_assignment_recalculate(
                    mcgrp);

        return member;
}

/**
 * @brief Destroys a consumer group member, removing from its consumer group.
 *
 * @param mcgrp Member consumer group.
 * @param member Member to destroy.
 *
 * @locks mcluster->lock MUST be held.
 */
static void rd_kafka_mock_cgrp_consumer_member_destroy(
    rd_kafka_mock_cgrp_consumer_t *mcgrp,
    rd_kafka_mock_cgrp_consumer_member_t *member) {
        rd_assert(mcgrp->member_cnt > 0);
        TAILQ_REMOVE(&mcgrp->members, member, link);
        mcgrp->member_cnt--;

        rd_kafka_mock_cgrp_consumer_target_assignment_recalculate(mcgrp);

        rd_free(member->id);

        if (member->instance_id)
                rd_free(member->instance_id);

        RD_IF_FREE(member->target_assignment,
                   rd_kafka_topic_partition_list_destroy);
        RD_IF_FREE(member->current_assignment,
                   rd_kafka_topic_partition_list_destroy);
        RD_IF_FREE(member->returned_assignment,
                   rd_kafka_topic_partition_list_destroy);
        RD_IF_FREE(member->subscribed_topics, rd_list_destroy_free);

        RD_IF_FREE(member->subscribed_topic_names, rd_list_destroy_free);

        RD_IF_FREE(member->subscribed_topic_regex, rd_free);

        rd_free(member);
}

static void rd_kafka_mock_cgrp_consumer_member_leave_static(
    rd_kafka_mock_cgrp_consumer_member_t *member) {
        member->left_static_membership = rd_true;
        rd_kafka_mock_cgrp_consumer_member_returned_assignment_set(member,
                                                                   NULL);
}


/**
 * @brief Called when a member must leave a consumer group.
 *
 * @param mcgrp Consumer group to leave.
 * @param member Member that leaves.
 * @param leave_static If true, the member is leaving with static group
 * membership.
 *
 * @locks mcluster->lock MUST be held.
 */
void rd_kafka_mock_cgrp_consumer_member_leave(
    rd_kafka_mock_cgrp_consumer_t *mcgrp,
    rd_kafka_mock_cgrp_consumer_member_t *member,
    rd_bool_t leave_static) {
        rd_bool_t is_static = member->instance_id != NULL;

        rd_kafka_dbg(mcgrp->cluster->rk, MOCK, "MOCK",
                     "Member %s is leaving group %s, is static: %s, "
                     "static leave: %s",
                     member->id, mcgrp->id, RD_STR_ToF(is_static),
                     RD_STR_ToF(leave_static));
        if (!is_static || !leave_static)
                rd_kafka_mock_cgrp_consumer_member_destroy(mcgrp, member);
        else
                rd_kafka_mock_cgrp_consumer_member_leave_static(member);
}

/**
 * @brief Called when a member is fenced from a consumer group.
 *
 * @param mcgrp Consumer group.
 * @param member Member to fence.
 *
 * @locks mcluster->lock MUST be held.
 */
void rd_kafka_mock_cgrp_consumer_member_fenced(
    rd_kafka_mock_cgrp_consumer_t *mcgrp,
    rd_kafka_mock_cgrp_consumer_member_t *member) {

        rd_kafka_dbg(mcgrp->cluster->rk, MOCK, "MOCK",
                     "Member %s is fenced from group %s", member->id,
                     mcgrp->id);

        rd_kafka_mock_cgrp_consumer_member_destroy(mcgrp, member);
}

/**
 * @brief Find a consumer group in cluster \p mcluster by \p GroupId.
 *
 * @param mcluster Cluster to search in.
 * @param GroupId Group id to search.
 * @return Found group or NULL.
 *
 * @locks mcluster->lock MUST be held.
 */
rd_kafka_mock_cgrp_consumer_t *
rd_kafka_mock_cgrp_consumer_find(const rd_kafka_mock_cluster_t *mcluster,
                                 const rd_kafkap_str_t *GroupId) {
        rd_kafka_mock_cgrp_consumer_t *mcgrp;
        TAILQ_FOREACH(mcgrp, &mcluster->cgrps_consumer, link) {
                if (!rd_kafkap_str_cmp_str(GroupId, mcgrp->id))
                        return mcgrp;
        }

        return NULL;
}

/**
 * @brief Check if any members have exceeded the session timeout.
 *
 * @param rkts Timers.
 * @param arg Consumer group.
 *
 * @locks mcluster->lock is acquired and released.
 */
static void rd_kafka_mock_cgrp_consumer_session_tmr_cb(rd_kafka_timers_t *rkts,
                                                       void *arg) {
        rd_kafka_mock_cgrp_consumer_t *mcgrp = arg;
        rd_kafka_mock_cgrp_consumer_member_t *member, *tmp;
        rd_ts_t now                       = rd_clock();
        rd_kafka_mock_cluster_t *mcluster = mcgrp->cluster;

        mtx_unlock(&mcluster->lock);
        TAILQ_FOREACH_SAFE(member, &mcgrp->members, link, tmp) {
                if (member->ts_last_activity +
                        (mcgrp->session_timeout_ms * 1000) >
                    now)
                        continue;

                rd_kafka_dbg(mcgrp->cluster->rk, MOCK, "MOCK",
                             "Member %s session timed out for group %s",
                             member->id, mcgrp->id);

                rd_kafka_mock_cgrp_consumer_member_fenced(mcgrp, member);
        }
        mtx_unlock(&mcluster->lock);
}


/**
 * @brief Find or create a "consumer" consumer group.
 *
 * @param mcluster Cluster to search in.
 * @param GroupId Group id to look for.
 * @return Found or new consumer group.
 *
 * @locks mcluster->lock MUST be held.
 */
rd_kafka_mock_cgrp_consumer_t *
rd_kafka_mock_cgrp_consumer_get(rd_kafka_mock_cluster_t *mcluster,
                                const rd_kafkap_str_t *GroupId) {
        rd_kafka_mock_cgrp_consumer_t *mcgrp;

        mcgrp = rd_kafka_mock_cgrp_consumer_find(mcluster, GroupId);
        if (mcgrp)
                return mcgrp;

        mcgrp              = rd_calloc(1, sizeof(*mcgrp));
        mcgrp->cluster     = mcluster;
        mcgrp->id          = RD_KAFKAP_STR_DUP(GroupId);
        mcgrp->group_epoch = 1;
        TAILQ_INIT(&mcgrp->members);
        rd_kafka_timer_start(&mcluster->timers, &mcgrp->session_tmr,
                             1000 * 1000 /*1s*/,
                             rd_kafka_mock_cgrp_consumer_session_tmr_cb, mcgrp);

        TAILQ_INSERT_TAIL(&mcluster->cgrps_consumer, mcgrp, link);

        return mcgrp;
}


void rd_kafka_mock_cgrp_consumer_target_assignment(
    rd_kafka_mock_cluster_t *mcluster,
    const char *group_id,
    rd_kafka_mock_cgrp_consumer_target_assignment_t *target_assignment) {
        rd_kafka_mock_cgrp_consumer_t *mcgrp;
        rd_kafkap_str_t *group_id_str =
            rd_kafkap_str_new(group_id, strlen(group_id));

        mtx_lock(&mcluster->lock);

        mcgrp = rd_kafka_mock_cgrp_consumer_find(mcluster, group_id_str);
        if (!mcgrp)
                goto destroy;

        rd_kafka_mock_cgrp_consumer_target_assignment_set_manual(
            mcgrp, target_assignment);

destroy:
        rd_kafkap_str_destroy(group_id_str);
        mtx_unlock(&mcluster->lock);
}

void rd_kafka_mock_set_group_consumer_session_timeout_ms(
    rd_kafka_mock_cluster_t *mcluster,
    int group_consumer_session_timeout_ms) {
        mtx_lock(&mcluster->lock);
        mcluster->defaults.group_consumer_session_timeout_ms =
            group_consumer_session_timeout_ms;
        mtx_unlock(&mcluster->lock);
}

void rd_kafka_mock_set_group_consumer_heartbeat_interval_ms(
    rd_kafka_mock_cluster_t *mcluster,
    int group_consumer_heartbeat_interval_ms) {
        mtx_lock(&mcluster->lock);
        mcluster->defaults.group_consumer_heartbeat_interval_ms =
            group_consumer_heartbeat_interval_ms;
        mtx_unlock(&mcluster->lock);
}

/**
 * @brief A client connection closed, check if any consumer cgrp has any state
 *        for this connection that needs to be cleared.
 *
 * @param mcluster Cluster to search in.
 * @param mconn Connection that was closed.
 *
 * @locks mcluster->lock MUST be held.
 */
void rd_kafka_mock_cgrps_consumer_connection_closed(
    rd_kafka_mock_cluster_t *mcluster,
    rd_kafka_mock_connection_t *mconn) {
        rd_kafka_mock_cgrp_consumer_t *mcgrp;

        TAILQ_FOREACH(mcgrp, &mcluster->cgrps_consumer, link) {
                rd_kafka_mock_cgrp_consumer_member_t *member, *tmp;
                TAILQ_FOREACH_SAFE(member, &mcgrp->members, link, tmp) {
                        if (member->conn == mconn) {
                                member->conn = NULL;
                                rd_kafka_mock_cgrp_consumer_member_returned_assignment_set(
                                    member, NULL);
                                rd_kafka_mock_cgrp_consumer_member_current_assignment_set(
                                    member, NULL);
                        }
                }
        }
}

/**
 * @brief Destroys consumer group \p mcgrp and all of its members.
 *
 * @param mcgrp Consumer group to destroy.
 *
 * @locks mcluster->lock MUST be held.
 */
void rd_kafka_mock_cgrp_consumer_destroy(rd_kafka_mock_cgrp_consumer_t *mcgrp) {
        rd_kafka_mock_cgrp_consumer_member_t *member;

        TAILQ_REMOVE(&mcgrp->cluster->cgrps_consumer, mcgrp, link);

        rd_kafka_timer_stop(&mcgrp->cluster->timers, &mcgrp->session_tmr,
                            rd_true);
        rd_free(mcgrp->id);
        while ((member = TAILQ_FIRST(&mcgrp->members)))
                rd_kafka_mock_cgrp_consumer_member_destroy(mcgrp, member);
        rd_free(mcgrp);
}

/**
 * @brief A client connection closed, check if any cgrp has any state
 *        for this connection that needs to be cleared.
 *
 * @param mcluster Mock cluster.
 * @param mconn Connection that was closed.
 */
void rd_kafka_mock_cgrps_connection_closed(rd_kafka_mock_cluster_t *mcluster,
                                           rd_kafka_mock_connection_t *mconn) {
        rd_kafka_mock_cgrps_classic_connection_closed(mcluster, mconn);
        rd_kafka_mock_cgrps_consumer_connection_closed(mcluster, mconn);
}
