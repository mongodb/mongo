/*
 * librdkafka - The Apache Kafka C/C++ library
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

#ifndef _RDKAFKA_PROTOCOL_H_
#define _RDKAFKA_PROTOCOL_H_

/**
 * Kafka protocol defines.
 *
 * The separation from rdkafka_proto.h is to provide the protocol defines
 * to C and C++ test code in tests/.
 */

#define RD_KAFKA_PORT     9092
#define RD_KAFKA_PORT_STR "9092"


/**
 * Request types
 *
 * Generate updates to this list with generate_proto.sh.
 */
#define RD_KAFKAP_None                         -1
#define RD_KAFKAP_Produce                      0
#define RD_KAFKAP_Fetch                        1
#define RD_KAFKAP_ListOffsets                  2
#define RD_KAFKAP_Metadata                     3
#define RD_KAFKAP_LeaderAndIsr                 4
#define RD_KAFKAP_StopReplica                  5
#define RD_KAFKAP_UpdateMetadata               6
#define RD_KAFKAP_ControlledShutdown           7
#define RD_KAFKAP_OffsetCommit                 8
#define RD_KAFKAP_OffsetFetch                  9
#define RD_KAFKAP_FindCoordinator              10
#define RD_KAFKAP_JoinGroup                    11
#define RD_KAFKAP_Heartbeat                    12
#define RD_KAFKAP_LeaveGroup                   13
#define RD_KAFKAP_SyncGroup                    14
#define RD_KAFKAP_DescribeGroups               15
#define RD_KAFKAP_ListGroups                   16
#define RD_KAFKAP_SaslHandshake                17
#define RD_KAFKAP_ApiVersion                   18
#define RD_KAFKAP_CreateTopics                 19
#define RD_KAFKAP_DeleteTopics                 20
#define RD_KAFKAP_DeleteRecords                21
#define RD_KAFKAP_InitProducerId               22
#define RD_KAFKAP_OffsetForLeaderEpoch         23
#define RD_KAFKAP_AddPartitionsToTxn           24
#define RD_KAFKAP_AddOffsetsToTxn              25
#define RD_KAFKAP_EndTxn                       26
#define RD_KAFKAP_WriteTxnMarkers              27
#define RD_KAFKAP_TxnOffsetCommit              28
#define RD_KAFKAP_DescribeAcls                 29
#define RD_KAFKAP_CreateAcls                   30
#define RD_KAFKAP_DeleteAcls                   31
#define RD_KAFKAP_DescribeConfigs              32
#define RD_KAFKAP_AlterConfigs                 33
#define RD_KAFKAP_AlterReplicaLogDirs          34
#define RD_KAFKAP_DescribeLogDirs              35
#define RD_KAFKAP_SaslAuthenticate             36
#define RD_KAFKAP_CreatePartitions             37
#define RD_KAFKAP_CreateDelegationToken        38
#define RD_KAFKAP_RenewDelegationToken         39
#define RD_KAFKAP_ExpireDelegationToken        40
#define RD_KAFKAP_DescribeDelegationToken      41
#define RD_KAFKAP_DeleteGroups                 42
#define RD_KAFKAP_ElectLeaders                 43
#define RD_KAFKAP_IncrementalAlterConfigs      44
#define RD_KAFKAP_AlterPartitionReassignments  45
#define RD_KAFKAP_ListPartitionReassignments   46
#define RD_KAFKAP_OffsetDelete                 47
#define RD_KAFKAP_DescribeClientQuotas         48
#define RD_KAFKAP_AlterClientQuotas            49
#define RD_KAFKAP_DescribeUserScramCredentials 50
#define RD_KAFKAP_AlterUserScramCredentials    51
#define RD_KAFKAP_Vote                         52
#define RD_KAFKAP_BeginQuorumEpoch             53
#define RD_KAFKAP_EndQuorumEpoch               54
#define RD_KAFKAP_DescribeQuorum               55
#define RD_KAFKAP_AlterIsr                     56
#define RD_KAFKAP_UpdateFeatures               57
#define RD_KAFKAP_Envelope                     58
#define RD_KAFKAP__NUM                         59


#endif /* _RDKAFKA_PROTOCOL_H_ */
