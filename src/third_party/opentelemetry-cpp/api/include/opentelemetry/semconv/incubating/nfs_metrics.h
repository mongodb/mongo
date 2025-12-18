/*
 * Copyright The OpenTelemetry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * DO NOT EDIT, this is an Auto-generated file from:
 * buildscripts/semantic-convention/templates/registry/semantic_metrics-h.j2
 */

#pragma once

#include "opentelemetry/common/macros.h"
#include "opentelemetry/metrics/meter.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace semconv
{
namespace nfs
{

/**
  Reports the count of kernel NFS client TCP segments and UDP datagrams handled.
  <p>
  Linux: this metric is taken from the Linux kernel's svc_stat.netudpcnt and svc_stat.nettcpcnt
  <p>
  counter
 */
static constexpr const char *kMetricNfsClientNetCount = "nfs.client.net.count";
static constexpr const char *descrMetricNfsClientNetCount =
    "Reports the count of kernel NFS client TCP segments and UDP datagrams handled.";
static constexpr const char *unitMetricNfsClientNetCount = "{record}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricNfsClientNetCount(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricNfsClientNetCount, descrMetricNfsClientNetCount,
                                    unitMetricNfsClientNetCount);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricNfsClientNetCount(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricNfsClientNetCount, descrMetricNfsClientNetCount,
                                    unitMetricNfsClientNetCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricNfsClientNetCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricNfsClientNetCount, descrMetricNfsClientNetCount,
                                             unitMetricNfsClientNetCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricNfsClientNetCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(
      kMetricNfsClientNetCount, descrMetricNfsClientNetCount, unitMetricNfsClientNetCount);
}

/**
  Reports the count of kernel NFS client TCP connections accepted.
  <p>
  Linux: this metric is taken from the Linux kernel's svc_stat.nettcpconn
  <p>
  counter
 */
static constexpr const char *kMetricNfsClientNetTcpConnectionAccepted =
    "nfs.client.net.tcp.connection.accepted";
static constexpr const char *descrMetricNfsClientNetTcpConnectionAccepted =
    "Reports the count of kernel NFS client TCP connections accepted.";
static constexpr const char *unitMetricNfsClientNetTcpConnectionAccepted = "{connection}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricNfsClientNetTcpConnectionAccepted(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricNfsClientNetTcpConnectionAccepted,
                                    descrMetricNfsClientNetTcpConnectionAccepted,
                                    unitMetricNfsClientNetTcpConnectionAccepted);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricNfsClientNetTcpConnectionAccepted(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricNfsClientNetTcpConnectionAccepted,
                                    descrMetricNfsClientNetTcpConnectionAccepted,
                                    unitMetricNfsClientNetTcpConnectionAccepted);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricNfsClientNetTcpConnectionAccepted(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricNfsClientNetTcpConnectionAccepted,
                                             descrMetricNfsClientNetTcpConnectionAccepted,
                                             unitMetricNfsClientNetTcpConnectionAccepted);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricNfsClientNetTcpConnectionAccepted(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricNfsClientNetTcpConnectionAccepted,
                                              descrMetricNfsClientNetTcpConnectionAccepted,
                                              unitMetricNfsClientNetTcpConnectionAccepted);
}

/**
  Reports the count of kernel NFSv4+ client operations.
  <p>
  counter
 */
static constexpr const char *kMetricNfsClientOperationCount = "nfs.client.operation.count";
static constexpr const char *descrMetricNfsClientOperationCount =
    "Reports the count of kernel NFSv4+ client operations.";
static constexpr const char *unitMetricNfsClientOperationCount = "{operation}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricNfsClientOperationCount(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricNfsClientOperationCount,
                                    descrMetricNfsClientOperationCount,
                                    unitMetricNfsClientOperationCount);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricNfsClientOperationCount(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricNfsClientOperationCount,
                                    descrMetricNfsClientOperationCount,
                                    unitMetricNfsClientOperationCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricNfsClientOperationCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricNfsClientOperationCount,
                                             descrMetricNfsClientOperationCount,
                                             unitMetricNfsClientOperationCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricNfsClientOperationCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricNfsClientOperationCount,
                                              descrMetricNfsClientOperationCount,
                                              unitMetricNfsClientOperationCount);
}

/**
  Reports the count of kernel NFS client procedures.
  <p>
  counter
 */
static constexpr const char *kMetricNfsClientProcedureCount = "nfs.client.procedure.count";
static constexpr const char *descrMetricNfsClientProcedureCount =
    "Reports the count of kernel NFS client procedures.";
static constexpr const char *unitMetricNfsClientProcedureCount = "{procedure}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricNfsClientProcedureCount(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricNfsClientProcedureCount,
                                    descrMetricNfsClientProcedureCount,
                                    unitMetricNfsClientProcedureCount);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricNfsClientProcedureCount(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricNfsClientProcedureCount,
                                    descrMetricNfsClientProcedureCount,
                                    unitMetricNfsClientProcedureCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricNfsClientProcedureCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricNfsClientProcedureCount,
                                             descrMetricNfsClientProcedureCount,
                                             unitMetricNfsClientProcedureCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricNfsClientProcedureCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricNfsClientProcedureCount,
                                              descrMetricNfsClientProcedureCount,
                                              unitMetricNfsClientProcedureCount);
}

/**
  Reports the count of kernel NFS client RPC authentication refreshes.
  <p>
  Linux: this metric is taken from the Linux kernel's svc_stat.rpcauthrefresh
  <p>
  counter
 */
static constexpr const char *kMetricNfsClientRpcAuthrefreshCount =
    "nfs.client.rpc.authrefresh.count";
static constexpr const char *descrMetricNfsClientRpcAuthrefreshCount =
    "Reports the count of kernel NFS client RPC authentication refreshes.";
static constexpr const char *unitMetricNfsClientRpcAuthrefreshCount = "{authrefresh}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricNfsClientRpcAuthrefreshCount(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricNfsClientRpcAuthrefreshCount,
                                    descrMetricNfsClientRpcAuthrefreshCount,
                                    unitMetricNfsClientRpcAuthrefreshCount);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricNfsClientRpcAuthrefreshCount(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricNfsClientRpcAuthrefreshCount,
                                    descrMetricNfsClientRpcAuthrefreshCount,
                                    unitMetricNfsClientRpcAuthrefreshCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricNfsClientRpcAuthrefreshCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricNfsClientRpcAuthrefreshCount,
                                             descrMetricNfsClientRpcAuthrefreshCount,
                                             unitMetricNfsClientRpcAuthrefreshCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricNfsClientRpcAuthrefreshCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricNfsClientRpcAuthrefreshCount,
                                              descrMetricNfsClientRpcAuthrefreshCount,
                                              unitMetricNfsClientRpcAuthrefreshCount);
}

/**
  Reports the count of kernel NFS client RPCs sent, regardless of whether they're accepted/rejected
  by the server. <p> Linux: this metric is taken from the Linux kernel's svc_stat.rpccnt <p> counter
 */
static constexpr const char *kMetricNfsClientRpcCount = "nfs.client.rpc.count";
static constexpr const char *descrMetricNfsClientRpcCount =
    "Reports the count of kernel NFS client RPCs sent, regardless of whether they're "
    "accepted/rejected by the server.";
static constexpr const char *unitMetricNfsClientRpcCount = "{request}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricNfsClientRpcCount(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricNfsClientRpcCount, descrMetricNfsClientRpcCount,
                                    unitMetricNfsClientRpcCount);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricNfsClientRpcCount(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricNfsClientRpcCount, descrMetricNfsClientRpcCount,
                                    unitMetricNfsClientRpcCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricNfsClientRpcCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricNfsClientRpcCount, descrMetricNfsClientRpcCount,
                                             unitMetricNfsClientRpcCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricNfsClientRpcCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(
      kMetricNfsClientRpcCount, descrMetricNfsClientRpcCount, unitMetricNfsClientRpcCount);
}

/**
  Reports the count of kernel NFS client RPC retransmits.
  <p>
  Linux: this metric is taken from the Linux kernel's svc_stat.rpcretrans
  <p>
  counter
 */
static constexpr const char *kMetricNfsClientRpcRetransmitCount = "nfs.client.rpc.retransmit.count";
static constexpr const char *descrMetricNfsClientRpcRetransmitCount =
    "Reports the count of kernel NFS client RPC retransmits.";
static constexpr const char *unitMetricNfsClientRpcRetransmitCount = "{retransmit}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricNfsClientRpcRetransmitCount(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricNfsClientRpcRetransmitCount,
                                    descrMetricNfsClientRpcRetransmitCount,
                                    unitMetricNfsClientRpcRetransmitCount);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricNfsClientRpcRetransmitCount(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricNfsClientRpcRetransmitCount,
                                    descrMetricNfsClientRpcRetransmitCount,
                                    unitMetricNfsClientRpcRetransmitCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricNfsClientRpcRetransmitCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricNfsClientRpcRetransmitCount,
                                             descrMetricNfsClientRpcRetransmitCount,
                                             unitMetricNfsClientRpcRetransmitCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricNfsClientRpcRetransmitCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricNfsClientRpcRetransmitCount,
                                              descrMetricNfsClientRpcRetransmitCount,
                                              unitMetricNfsClientRpcRetransmitCount);
}

/**
  Reports the count of kernel NFS server stale file handles.
  <p>
  Linux: this metric is taken from the Linux kernel NFSD_STATS_FH_STALE counter in the nfsd_net
  struct <p> counter
 */
static constexpr const char *kMetricNfsServerFhStaleCount = "nfs.server.fh.stale.count";
static constexpr const char *descrMetricNfsServerFhStaleCount =
    "Reports the count of kernel NFS server stale file handles.";
static constexpr const char *unitMetricNfsServerFhStaleCount = "{fh}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricNfsServerFhStaleCount(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricNfsServerFhStaleCount, descrMetricNfsServerFhStaleCount,
                                    unitMetricNfsServerFhStaleCount);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricNfsServerFhStaleCount(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricNfsServerFhStaleCount, descrMetricNfsServerFhStaleCount,
                                    unitMetricNfsServerFhStaleCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricNfsServerFhStaleCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricNfsServerFhStaleCount,
                                             descrMetricNfsServerFhStaleCount,
                                             unitMetricNfsServerFhStaleCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricNfsServerFhStaleCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricNfsServerFhStaleCount,
                                              descrMetricNfsServerFhStaleCount,
                                              unitMetricNfsServerFhStaleCount);
}

/**
  Reports the count of kernel NFS server bytes returned to receive and transmit (read and write)
  requests. <p> Linux: this metric is taken from the Linux kernel NFSD_STATS_IO_READ and
  NFSD_STATS_IO_WRITE counters in the nfsd_net struct <p> counter
 */
static constexpr const char *kMetricNfsServerIo = "nfs.server.io";
static constexpr const char *descrMetricNfsServerIo =
    "Reports the count of kernel NFS server bytes returned to receive and transmit (read and "
    "write) requests.";
static constexpr const char *unitMetricNfsServerIo = "By";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricNfsServerIo(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricNfsServerIo, descrMetricNfsServerIo,
                                    unitMetricNfsServerIo);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricNfsServerIo(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricNfsServerIo, descrMetricNfsServerIo,
                                    unitMetricNfsServerIo);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncInt64MetricNfsServerIo(
    metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricNfsServerIo, descrMetricNfsServerIo,
                                             unitMetricNfsServerIo);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument> CreateAsyncDoubleMetricNfsServerIo(
    metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricNfsServerIo, descrMetricNfsServerIo,
                                              unitMetricNfsServerIo);
}

/**
  Reports the count of kernel NFS server TCP segments and UDP datagrams handled.
  <p>
  Linux: this metric is taken from the Linux kernel's svc_stat.nettcpcnt and svc_stat.netudpcnt
  <p>
  counter
 */
static constexpr const char *kMetricNfsServerNetCount = "nfs.server.net.count";
static constexpr const char *descrMetricNfsServerNetCount =
    "Reports the count of kernel NFS server TCP segments and UDP datagrams handled.";
static constexpr const char *unitMetricNfsServerNetCount = "{record}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricNfsServerNetCount(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricNfsServerNetCount, descrMetricNfsServerNetCount,
                                    unitMetricNfsServerNetCount);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricNfsServerNetCount(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricNfsServerNetCount, descrMetricNfsServerNetCount,
                                    unitMetricNfsServerNetCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricNfsServerNetCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricNfsServerNetCount, descrMetricNfsServerNetCount,
                                             unitMetricNfsServerNetCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricNfsServerNetCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(
      kMetricNfsServerNetCount, descrMetricNfsServerNetCount, unitMetricNfsServerNetCount);
}

/**
  Reports the count of kernel NFS server TCP connections accepted.
  <p>
  Linux: this metric is taken from the Linux kernel's svc_stat.nettcpconn
  <p>
  counter
 */
static constexpr const char *kMetricNfsServerNetTcpConnectionAccepted =
    "nfs.server.net.tcp.connection.accepted";
static constexpr const char *descrMetricNfsServerNetTcpConnectionAccepted =
    "Reports the count of kernel NFS server TCP connections accepted.";
static constexpr const char *unitMetricNfsServerNetTcpConnectionAccepted = "{connection}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricNfsServerNetTcpConnectionAccepted(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricNfsServerNetTcpConnectionAccepted,
                                    descrMetricNfsServerNetTcpConnectionAccepted,
                                    unitMetricNfsServerNetTcpConnectionAccepted);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricNfsServerNetTcpConnectionAccepted(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricNfsServerNetTcpConnectionAccepted,
                                    descrMetricNfsServerNetTcpConnectionAccepted,
                                    unitMetricNfsServerNetTcpConnectionAccepted);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricNfsServerNetTcpConnectionAccepted(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricNfsServerNetTcpConnectionAccepted,
                                             descrMetricNfsServerNetTcpConnectionAccepted,
                                             unitMetricNfsServerNetTcpConnectionAccepted);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricNfsServerNetTcpConnectionAccepted(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricNfsServerNetTcpConnectionAccepted,
                                              descrMetricNfsServerNetTcpConnectionAccepted,
                                              unitMetricNfsServerNetTcpConnectionAccepted);
}

/**
  Reports the count of kernel NFSv4+ server operations.
  <p>
  counter
 */
static constexpr const char *kMetricNfsServerOperationCount = "nfs.server.operation.count";
static constexpr const char *descrMetricNfsServerOperationCount =
    "Reports the count of kernel NFSv4+ server operations.";
static constexpr const char *unitMetricNfsServerOperationCount = "{operation}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricNfsServerOperationCount(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricNfsServerOperationCount,
                                    descrMetricNfsServerOperationCount,
                                    unitMetricNfsServerOperationCount);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricNfsServerOperationCount(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricNfsServerOperationCount,
                                    descrMetricNfsServerOperationCount,
                                    unitMetricNfsServerOperationCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricNfsServerOperationCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricNfsServerOperationCount,
                                             descrMetricNfsServerOperationCount,
                                             unitMetricNfsServerOperationCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricNfsServerOperationCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricNfsServerOperationCount,
                                              descrMetricNfsServerOperationCount,
                                              unitMetricNfsServerOperationCount);
}

/**
  Reports the count of kernel NFS server procedures.
  <p>
  counter
 */
static constexpr const char *kMetricNfsServerProcedureCount = "nfs.server.procedure.count";
static constexpr const char *descrMetricNfsServerProcedureCount =
    "Reports the count of kernel NFS server procedures.";
static constexpr const char *unitMetricNfsServerProcedureCount = "{procedure}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricNfsServerProcedureCount(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricNfsServerProcedureCount,
                                    descrMetricNfsServerProcedureCount,
                                    unitMetricNfsServerProcedureCount);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricNfsServerProcedureCount(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricNfsServerProcedureCount,
                                    descrMetricNfsServerProcedureCount,
                                    unitMetricNfsServerProcedureCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricNfsServerProcedureCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricNfsServerProcedureCount,
                                             descrMetricNfsServerProcedureCount,
                                             unitMetricNfsServerProcedureCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricNfsServerProcedureCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricNfsServerProcedureCount,
                                              descrMetricNfsServerProcedureCount,
                                              unitMetricNfsServerProcedureCount);
}

/**
  Reports the kernel NFS server reply cache request count by cache hit status.
  <p>
  counter
 */
static constexpr const char *kMetricNfsServerRepcacheRequests = "nfs.server.repcache.requests";
static constexpr const char *descrMetricNfsServerRepcacheRequests =
    "Reports the kernel NFS server reply cache request count by cache hit status.";
static constexpr const char *unitMetricNfsServerRepcacheRequests = "{request}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>>
CreateSyncInt64MetricNfsServerRepcacheRequests(metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricNfsServerRepcacheRequests,
                                    descrMetricNfsServerRepcacheRequests,
                                    unitMetricNfsServerRepcacheRequests);
}

static inline nostd::unique_ptr<metrics::Counter<double>>
CreateSyncDoubleMetricNfsServerRepcacheRequests(metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricNfsServerRepcacheRequests,
                                    descrMetricNfsServerRepcacheRequests,
                                    unitMetricNfsServerRepcacheRequests);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricNfsServerRepcacheRequests(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricNfsServerRepcacheRequests,
                                             descrMetricNfsServerRepcacheRequests,
                                             unitMetricNfsServerRepcacheRequests);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricNfsServerRepcacheRequests(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(kMetricNfsServerRepcacheRequests,
                                              descrMetricNfsServerRepcacheRequests,
                                              unitMetricNfsServerRepcacheRequests);
}

/**
  Reports the count of kernel NFS server RPCs handled.
  <p>
  Linux: this metric is taken from the Linux kernel's svc_stat.rpccnt, the count of good RPCs. This
  metric can have an error.type of "format", "auth", or "client" for svc_stat.badfmt,
  svc_stat.badauth, and svc_stat.badclnt. <p> counter
 */
static constexpr const char *kMetricNfsServerRpcCount = "nfs.server.rpc.count";
static constexpr const char *descrMetricNfsServerRpcCount =
    "Reports the count of kernel NFS server RPCs handled.";
static constexpr const char *unitMetricNfsServerRpcCount = "{request}";

static inline nostd::unique_ptr<metrics::Counter<uint64_t>> CreateSyncInt64MetricNfsServerRpcCount(
    metrics::Meter *meter)
{
  return meter->CreateUInt64Counter(kMetricNfsServerRpcCount, descrMetricNfsServerRpcCount,
                                    unitMetricNfsServerRpcCount);
}

static inline nostd::unique_ptr<metrics::Counter<double>> CreateSyncDoubleMetricNfsServerRpcCount(
    metrics::Meter *meter)
{
  return meter->CreateDoubleCounter(kMetricNfsServerRpcCount, descrMetricNfsServerRpcCount,
                                    unitMetricNfsServerRpcCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricNfsServerRpcCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableCounter(kMetricNfsServerRpcCount, descrMetricNfsServerRpcCount,
                                             unitMetricNfsServerRpcCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricNfsServerRpcCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableCounter(
      kMetricNfsServerRpcCount, descrMetricNfsServerRpcCount, unitMetricNfsServerRpcCount);
}

/**
  Reports the count of kernel NFS server available threads.
  <p>
  Linux: this metric is taken from the Linux kernel nfsd_th_cnt variable
  <p>
  updowncounter
 */
static constexpr const char *kMetricNfsServerThreadCount = "nfs.server.thread.count";
static constexpr const char *descrMetricNfsServerThreadCount =
    "Reports the count of kernel NFS server available threads.";
static constexpr const char *unitMetricNfsServerThreadCount = "{thread}";

static inline nostd::unique_ptr<metrics::UpDownCounter<int64_t>>
CreateSyncInt64MetricNfsServerThreadCount(metrics::Meter *meter)
{
  return meter->CreateInt64UpDownCounter(
      kMetricNfsServerThreadCount, descrMetricNfsServerThreadCount, unitMetricNfsServerThreadCount);
}

static inline nostd::unique_ptr<metrics::UpDownCounter<double>>
CreateSyncDoubleMetricNfsServerThreadCount(metrics::Meter *meter)
{
  return meter->CreateDoubleUpDownCounter(
      kMetricNfsServerThreadCount, descrMetricNfsServerThreadCount, unitMetricNfsServerThreadCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncInt64MetricNfsServerThreadCount(metrics::Meter *meter)
{
  return meter->CreateInt64ObservableUpDownCounter(
      kMetricNfsServerThreadCount, descrMetricNfsServerThreadCount, unitMetricNfsServerThreadCount);
}

static inline nostd::shared_ptr<metrics::ObservableInstrument>
CreateAsyncDoubleMetricNfsServerThreadCount(metrics::Meter *meter)
{
  return meter->CreateDoubleObservableUpDownCounter(
      kMetricNfsServerThreadCount, descrMetricNfsServerThreadCount, unitMetricNfsServerThreadCount);
}

}  // namespace nfs
}  // namespace semconv
OPENTELEMETRY_END_NAMESPACE
