#pragma once

#ifndef __linux__

#include "src/stirling/source_connector.h"

namespace pl {
namespace stirling {

DUMMY_SOURCE_CONNECTOR(SocketTraceConnector);

}  // namespace stirling
}  // namespace pl

#else

#include <bcc/BPF.h>

#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "demos/applications/hipster_shop/reflection.h"
#include "src/common/grpcutils/service_descriptor_database.h"
#include "src/common/system/socket_info.h"
#include "src/stirling/bpf_tools/bcc_wrapper.h"
#include "src/stirling/connection_tracker.h"
#include "src/stirling/http_table.h"
#include "src/stirling/mysql_table.h"
#include "src/stirling/socket_trace.h"
#include "src/stirling/source_connector.h"

DECLARE_string(http_response_header_filters);
DECLARE_bool(stirling_enable_parsing_protobufs);
DECLARE_uint32(stirling_socket_trace_sampling_period_millis);
DECLARE_string(perf_buffer_events_output_path);
DECLARE_bool(stirling_enable_http_tracing);
DECLARE_bool(stirling_enable_grpc_tracing);
DECLARE_bool(stirling_enable_mysql_tracing);
DECLARE_bool(stirling_disable_self_tracing);

BCC_SRC_STRVIEW(http_trace_bcc_script, socket_trace);

namespace pl {
namespace stirling {

enum class HTTPContentType {
  kUnknown = 0,
  kJSON = 1,
  // We use gRPC instead of PB to be consistent with the wording used in gRPC.
  kGRPC = 2,
};

class SocketTraceConnector : public SourceConnector, public bpf_tools::BCCWrapper {
 public:
  inline static const std::string_view kBCCScript = http_trace_bcc_script;

  static constexpr std::string_view kHTTPPerfBufferNames[] = {
      "socket_control_events",
      "socket_data_events",
  };

  // Used in ReadPerfBuffer to drain the relevant perf buffers.
  static constexpr auto kHTTPPerfBuffers = ArrayView<std::string_view>(kHTTPPerfBufferNames);

  static constexpr std::string_view kMySQLPerfBufferNames[] = {
      "socket_control_events",
      "socket_data_events",
  };

  static constexpr auto kMySQLPerfBuffers = ArrayView<std::string_view>(kMySQLPerfBufferNames);

  static constexpr DataTableSchema kTablesArray[] = {kHTTPTable, kMySQLTable};
  static constexpr auto kTables = ArrayView<DataTableSchema>(kTablesArray);
  static constexpr uint32_t kHTTPTableNum = SourceConnector::TableNum(kTables, kHTTPTable);
  static constexpr uint32_t kMySQLTableNum = SourceConnector::TableNum(kTables, kMySQLTable);

  static constexpr std::chrono::milliseconds kDefaultPushPeriod{1000};

  // Dim 0: DataTables; dim 1: perfBuffer Names
  static constexpr ArrayView<std::string_view> perfBufferNames[] = {kHTTPPerfBuffers,
                                                                    kMySQLPerfBuffers};
  // TODO(yzhao/oazizi): This is no longer necessary because different tables now pull data from the
  // same set of perf buffers. But we'd need to think about how to adapt the APIs with the table_num
  // argument.
  static constexpr auto kTablePerfBufferMap =
      ArrayView<ArrayView<std::string_view> >(perfBufferNames);

  static std::unique_ptr<SourceConnector> Create(std::string_view name) {
    return std::unique_ptr<SourceConnector>(new SocketTraceConnector(name));
  }

  Status InitImpl() override;
  Status StopImpl() override;
  void TransferDataImpl(ConnectorContext* ctx, uint32_t table_num, DataTable* data_table) override;

  Status Configure(TrafficProtocol protocol, uint64_t config_mask);
  Status TestOnlySetTargetPID(int64_t pid);
  Status DisableSelfTracing();

  /**
   * @brief Number of active ConnectionTrackers.
   *
   * Note: Multiple ConnectionTrackers on same TGID+FD are counted as 1.
   */
  size_t NumActiveConnections() const { return connection_trackers_.size(); }

  /**
   * @brief Gets a pointer to a ConnectionTracker by conn_id.
   *
   * @param connid The connection to get.
   * @return Pointer to the ConnectionTracker, or nullptr if it does not exist.
   */
  const ConnectionTracker* GetConnectionTracker(struct conn_id_t connid) const;

  static void TestOnlySetHTTPResponseHeaderFilter(http::HTTPHeaderFilter filter) {
    http_response_header_filter_ = std::move(filter);
  }

  // This function causes the perf buffer to be read, and triggers callbacks per message.
  // TODO(oazizi): This function is only public for testing purposes. Make private?
  void ReadPerfBuffer(uint32_t table_num);

 private:
  // ReadPerfBuffer poll callback functions (must be static).
  // These are used by the static variables below, and have to be placed here.
  static void HandleDataEvent(void* cb_cookie, void* data, int data_size);
  static void HandleDataEventsLoss(void* cb_cookie, uint64_t lost);
  static void HandleControlEvent(void* cb_cookie, void* data, int data_size);
  static void HandleControlEventsLoss(void* cb_cookie, uint64_t lost);

  static constexpr bpf_tools::KProbeSpec kProbeSpecsArray[] = {
      {"connect", "syscall__probe_entry_connect", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"connect", "syscall__probe_ret_connect", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"accept", "syscall__probe_entry_accept", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"accept", "syscall__probe_ret_accept", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"accept4", "syscall__probe_entry_accept4", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"accept4", "syscall__probe_ret_accept4", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"open", "syscall__probe_ret_open", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"creat", "syscall__probe_ret_open", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"openat", "syscall__probe_ret_open", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"write", "syscall__probe_entry_write", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"write", "syscall__probe_ret_write", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"writev", "syscall__probe_entry_writev", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"writev", "syscall__probe_ret_writev", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"send", "syscall__probe_entry_send", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"send", "syscall__probe_ret_send", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"sendto", "syscall__probe_entry_sendto", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"sendto", "syscall__probe_ret_sendto", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"sendmsg", "syscall__probe_entry_sendmsg", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"sendmsg", "syscall__probe_ret_sendmsg", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"read", "syscall__probe_entry_read", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"read", "syscall__probe_ret_read", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"readv", "syscall__probe_entry_readv", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"readv", "syscall__probe_ret_readv", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"recv", "syscall__probe_entry_recv", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"recv", "syscall__probe_ret_recv", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"recvfrom", "syscall__probe_entry_recv", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"recvfrom", "syscall__probe_ret_recv", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"recvmsg", "syscall__probe_entry_recvmsg", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"recvmsg", "syscall__probe_ret_recvmsg", bpf_probe_attach_type::BPF_PROBE_RETURN},
      {"close", "syscall__probe_entry_close", bpf_probe_attach_type::BPF_PROBE_ENTRY},
      {"close", "syscall__probe_ret_close", bpf_probe_attach_type::BPF_PROBE_RETURN},
  };
  static constexpr auto kProbeSpecs = ArrayView<bpf_tools::KProbeSpec>(kProbeSpecsArray);

  // TODO(oazizi): Remove send and recv probes once we are confident that they don't trace anything.
  //               Note that send/recv are not in the syscall table
  //               (https://filippo.io/linux-syscall-table/), but are defined as SYSCALL_DEFINE4 in
  //               https://elixir.bootlin.com/linux/latest/source/net/socket.c.

  static constexpr bpf_tools::PerfBufferSpec kPerfBufferSpecsArray[] = {
      // For data events. The order must be consistent with output tables.
      {"socket_data_events", &SocketTraceConnector::HandleDataEvent,
       &SocketTraceConnector::HandleDataEventsLoss},
      // For non-data events. Must not mix with the above perf buffers for data events.
      {"socket_control_events", &SocketTraceConnector::HandleControlEvent,
       &SocketTraceConnector::HandleControlEventsLoss},
  };
  static constexpr auto kPerfBufferSpecs =
      ArrayView<bpf_tools::PerfBufferSpec>(kPerfBufferSpecsArray);

  inline static http::HTTPHeaderFilter http_response_header_filter_;
  // TODO(yzhao): We will remove this once finalized the mechanism of lazy protobuf parse.
  inline static ::pl::grpc::ServiceDescriptorDatabase grpc_desc_db_{
      demos::hipster_shop::GetFileDescriptorSet()};

  explicit SocketTraceConnector(std::string_view source_name)
      : SourceConnector(
            source_name, kTables,
            std::chrono::milliseconds(FLAGS_stirling_socket_trace_sampling_period_millis),
            kDefaultPushPeriod),
        bpf_tools::BCCWrapper(kBCCScript) {
    // TODO(yzhao): Is there a better place/time to grab the flags?
    http_response_header_filter_ = http::ParseHTTPHeaderFilters(FLAGS_http_response_header_filters);
    proc_parser_ = std::make_unique<system::ProcParser>(system::Config::GetInstance());
    netlink_socket_prober_ = std::make_unique<system::NetlinkSocketProber>();
  }

  // Events from BPF.
  // TODO(oazizi/yzhao): These all operate based on pass-by-value, which copies.
  //                     The Handle* functions should call make_unique() of new corresponding
  //                     objects, and these functions should take unique_ptrs.
  void AcceptDataEvent(std::unique_ptr<SocketDataEvent> event);
  void AcceptControlEvent(const socket_control_event_t& event);

  // Transfer of messages to the data table.
  template <typename TEntryType>
  void TransferStreams(ConnectorContext* ctx, TrafficProtocol protocol, DataTable* data_table);

  template <typename TEntryType>
  static void AppendMessage(ConnectorContext* ctx, const ConnectionTracker& conn_tracker,
                            TEntryType record, DataTable* data_table);

  // HTTP-specific helper function.
  static bool SelectMessage(const http::Record& record);

  // TODO(oazizi/yzhao): Change to use std::unique_ptr.

  // Note that the inner map cannot be a vector, because there is no guaranteed order
  // in which events are read from perf buffers.
  // Inner map could be a priority_queue, but benchmarks showed better performance with a std::map.
  // Key is {PID, FD} for outer map (see GetStreamId()), and generation for inner map.
  std::unordered_map<uint64_t, std::map<uint64_t, ConnectionTracker> > connection_trackers_;

  // If not a nullptr, writes the events received from perf buffers to this stream.
  std::unique_ptr<std::ofstream> perf_buffer_events_output_stream_;

  std::unique_ptr<system::NetlinkSocketProber> netlink_socket_prober_;

  std::unique_ptr<std::map<int, system::SocketInfo> > socket_connections_;

  std::unique_ptr<system::ProcParser> proc_parser_;

  FRIEND_TEST(SocketTraceConnectorTest, AppendNonContiguousEvents);
  FRIEND_TEST(SocketTraceConnectorTest, NoEvents);
  FRIEND_TEST(SocketTraceConnectorTest, End2End);
  FRIEND_TEST(SocketTraceConnectorTest, UPIDCheck);
  FRIEND_TEST(SocketTraceConnectorTest, RequestResponseMatching);
  FRIEND_TEST(SocketTraceConnectorTest, MissingEventInStream);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupInOrder);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupOutOfOrder);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupMissingDataEvent);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupOldGenerations);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupInactiveDead);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupInactiveAlive);
  FRIEND_TEST(SocketTraceConnectorTest, ConnectionCleanupNoProtocol);
  FRIEND_TEST(SocketTraceConnectorTest, MySQLPrepareExecuteClose);
  FRIEND_TEST(SocketTraceConnectorTest, MySQLQuery);
  FRIEND_TEST(SocketTraceConnectorTest, MySQLMultipleCommands);
  FRIEND_TEST(SocketTraceConnectorTest, MySQLQueryWithLargeResultset);
  FRIEND_TEST(SocketTraceConnectorTest, MySQLMultiResultset);
};

}  // namespace stirling
}  // namespace pl

#endif
