// Autogenerated by the ProtoZero compiler plugin. DO NOT EDIT.

#ifndef PERFETTO_PROTOS_TRACE_CONFIG_PROTO_H_
#define PERFETTO_PROTOS_TRACE_CONFIG_PROTO_H_

#include <stddef.h>
#include <stdint.h>

#include "perfetto/protozero/message.h"
#include "perfetto/protozero/packed_repeated_fields.h"
#include "perfetto/protozero/proto_decoder.h"
#include "perfetto/protozero/proto_utils.h"

namespace perfetto {
namespace protos {
namespace pbzero {

class DataSourceConfig;
class TraceConfig_BufferConfig;
class TraceConfig_BuiltinDataSource;
class TraceConfig_DataSource;
class TraceConfig_GuardrailOverrides;
class TraceConfig_IncidentReportConfig;
class TraceConfig_IncrementalStateConfig;
class TraceConfig_ProducerConfig;
class TraceConfig_StatsdMetadata;
class TraceConfig_TriggerConfig;
class TraceConfig_TriggerConfig_Trigger;
enum BuiltinClock : int32_t;
enum TraceConfig_BufferConfig_FillPolicy : int32_t;
enum TraceConfig_CompressionType : int32_t;
enum TraceConfig_LockdownModeOperation : int32_t;
enum TraceConfig_TriggerConfig_TriggerMode : int32_t;

enum TraceConfig_LockdownModeOperation : int32_t {
  TraceConfig_LockdownModeOperation_LOCKDOWN_UNCHANGED = 0,
  TraceConfig_LockdownModeOperation_LOCKDOWN_CLEAR = 1,
  TraceConfig_LockdownModeOperation_LOCKDOWN_SET = 2,
};

const TraceConfig_LockdownModeOperation TraceConfig_LockdownModeOperation_MIN = TraceConfig_LockdownModeOperation_LOCKDOWN_UNCHANGED;
const TraceConfig_LockdownModeOperation TraceConfig_LockdownModeOperation_MAX = TraceConfig_LockdownModeOperation_LOCKDOWN_SET;

enum TraceConfig_CompressionType : int32_t {
  TraceConfig_CompressionType_COMPRESSION_TYPE_UNSPECIFIED = 0,
  TraceConfig_CompressionType_COMPRESSION_TYPE_DEFLATE = 1,
};

const TraceConfig_CompressionType TraceConfig_CompressionType_MIN = TraceConfig_CompressionType_COMPRESSION_TYPE_UNSPECIFIED;
const TraceConfig_CompressionType TraceConfig_CompressionType_MAX = TraceConfig_CompressionType_COMPRESSION_TYPE_DEFLATE;

enum TraceConfig_TriggerConfig_TriggerMode : int32_t {
  TraceConfig_TriggerConfig_TriggerMode_UNSPECIFIED = 0,
  TraceConfig_TriggerConfig_TriggerMode_START_TRACING = 1,
  TraceConfig_TriggerConfig_TriggerMode_STOP_TRACING = 2,
};

const TraceConfig_TriggerConfig_TriggerMode TraceConfig_TriggerConfig_TriggerMode_MIN = TraceConfig_TriggerConfig_TriggerMode_UNSPECIFIED;
const TraceConfig_TriggerConfig_TriggerMode TraceConfig_TriggerConfig_TriggerMode_MAX = TraceConfig_TriggerConfig_TriggerMode_STOP_TRACING;

enum TraceConfig_BufferConfig_FillPolicy : int32_t {
  TraceConfig_BufferConfig_FillPolicy_UNSPECIFIED = 0,
  TraceConfig_BufferConfig_FillPolicy_RING_BUFFER = 1,
  TraceConfig_BufferConfig_FillPolicy_DISCARD = 2,
};

const TraceConfig_BufferConfig_FillPolicy TraceConfig_BufferConfig_FillPolicy_MIN = TraceConfig_BufferConfig_FillPolicy_UNSPECIFIED;
const TraceConfig_BufferConfig_FillPolicy TraceConfig_BufferConfig_FillPolicy_MAX = TraceConfig_BufferConfig_FillPolicy_DISCARD;

class TraceConfig_Decoder : public ::protozero::TypedProtoDecoder</*MAX_FIELD_ID=*/29, /*HAS_NONPACKED_REPEATED_FIELDS=*/true> {
 public:
  TraceConfig_Decoder(const uint8_t* data, size_t len) : TypedProtoDecoder(data, len) {}
  explicit TraceConfig_Decoder(const std::string& raw) : TypedProtoDecoder(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()) {}
  explicit TraceConfig_Decoder(const ::protozero::ConstBytes& raw) : TypedProtoDecoder(raw.data, raw.size) {}
  bool has_buffers() const { return at<1>().valid(); }
  ::protozero::RepeatedFieldIterator<::protozero::ConstBytes> buffers() const { return GetRepeated<::protozero::ConstBytes>(1); }
  bool has_data_sources() const { return at<2>().valid(); }
  ::protozero::RepeatedFieldIterator<::protozero::ConstBytes> data_sources() const { return GetRepeated<::protozero::ConstBytes>(2); }
  bool has_builtin_data_sources() const { return at<20>().valid(); }
  ::protozero::ConstBytes builtin_data_sources() const { return at<20>().as_bytes(); }
  bool has_duration_ms() const { return at<3>().valid(); }
  uint32_t duration_ms() const { return at<3>().as_uint32(); }
  bool has_enable_extra_guardrails() const { return at<4>().valid(); }
  bool enable_extra_guardrails() const { return at<4>().as_bool(); }
  bool has_lockdown_mode() const { return at<5>().valid(); }
  int32_t lockdown_mode() const { return at<5>().as_int32(); }
  bool has_producers() const { return at<6>().valid(); }
  ::protozero::RepeatedFieldIterator<::protozero::ConstBytes> producers() const { return GetRepeated<::protozero::ConstBytes>(6); }
  bool has_statsd_metadata() const { return at<7>().valid(); }
  ::protozero::ConstBytes statsd_metadata() const { return at<7>().as_bytes(); }
  bool has_write_into_file() const { return at<8>().valid(); }
  bool write_into_file() const { return at<8>().as_bool(); }
  bool has_output_path() const { return at<29>().valid(); }
  ::protozero::ConstChars output_path() const { return at<29>().as_string(); }
  bool has_file_write_period_ms() const { return at<9>().valid(); }
  uint32_t file_write_period_ms() const { return at<9>().as_uint32(); }
  bool has_max_file_size_bytes() const { return at<10>().valid(); }
  uint64_t max_file_size_bytes() const { return at<10>().as_uint64(); }
  bool has_guardrail_overrides() const { return at<11>().valid(); }
  ::protozero::ConstBytes guardrail_overrides() const { return at<11>().as_bytes(); }
  bool has_deferred_start() const { return at<12>().valid(); }
  bool deferred_start() const { return at<12>().as_bool(); }
  bool has_flush_period_ms() const { return at<13>().valid(); }
  uint32_t flush_period_ms() const { return at<13>().as_uint32(); }
  bool has_flush_timeout_ms() const { return at<14>().valid(); }
  uint32_t flush_timeout_ms() const { return at<14>().as_uint32(); }
  bool has_data_source_stop_timeout_ms() const { return at<23>().valid(); }
  uint32_t data_source_stop_timeout_ms() const { return at<23>().as_uint32(); }
  bool has_notify_traceur() const { return at<16>().valid(); }
  bool notify_traceur() const { return at<16>().as_bool(); }
  bool has_trigger_config() const { return at<17>().valid(); }
  ::protozero::ConstBytes trigger_config() const { return at<17>().as_bytes(); }
  bool has_activate_triggers() const { return at<18>().valid(); }
  ::protozero::RepeatedFieldIterator<::protozero::ConstChars> activate_triggers() const { return GetRepeated<::protozero::ConstChars>(18); }
  bool has_incremental_state_config() const { return at<21>().valid(); }
  ::protozero::ConstBytes incremental_state_config() const { return at<21>().as_bytes(); }
  bool has_allow_user_build_tracing() const { return at<19>().valid(); }
  bool allow_user_build_tracing() const { return at<19>().as_bool(); }
  bool has_unique_session_name() const { return at<22>().valid(); }
  ::protozero::ConstChars unique_session_name() const { return at<22>().as_string(); }
  bool has_compression_type() const { return at<24>().valid(); }
  int32_t compression_type() const { return at<24>().as_int32(); }
  bool has_incident_report_config() const { return at<25>().valid(); }
  ::protozero::ConstBytes incident_report_config() const { return at<25>().as_bytes(); }
  bool has_trace_uuid_msb() const { return at<27>().valid(); }
  int64_t trace_uuid_msb() const { return at<27>().as_int64(); }
  bool has_trace_uuid_lsb() const { return at<28>().valid(); }
  int64_t trace_uuid_lsb() const { return at<28>().as_int64(); }
};

class TraceConfig : public ::protozero::Message {
 public:
  using Decoder = TraceConfig_Decoder;
  enum : int32_t {
    kBuffersFieldNumber = 1,
    kDataSourcesFieldNumber = 2,
    kBuiltinDataSourcesFieldNumber = 20,
    kDurationMsFieldNumber = 3,
    kEnableExtraGuardrailsFieldNumber = 4,
    kLockdownModeFieldNumber = 5,
    kProducersFieldNumber = 6,
    kStatsdMetadataFieldNumber = 7,
    kWriteIntoFileFieldNumber = 8,
    kOutputPathFieldNumber = 29,
    kFileWritePeriodMsFieldNumber = 9,
    kMaxFileSizeBytesFieldNumber = 10,
    kGuardrailOverridesFieldNumber = 11,
    kDeferredStartFieldNumber = 12,
    kFlushPeriodMsFieldNumber = 13,
    kFlushTimeoutMsFieldNumber = 14,
    kDataSourceStopTimeoutMsFieldNumber = 23,
    kNotifyTraceurFieldNumber = 16,
    kTriggerConfigFieldNumber = 17,
    kActivateTriggersFieldNumber = 18,
    kIncrementalStateConfigFieldNumber = 21,
    kAllowUserBuildTracingFieldNumber = 19,
    kUniqueSessionNameFieldNumber = 22,
    kCompressionTypeFieldNumber = 24,
    kIncidentReportConfigFieldNumber = 25,
    kTraceUuidMsbFieldNumber = 27,
    kTraceUuidLsbFieldNumber = 28,
  };
  using BufferConfig = ::perfetto::protos::pbzero::TraceConfig_BufferConfig;
  using DataSource = ::perfetto::protos::pbzero::TraceConfig_DataSource;
  using BuiltinDataSource = ::perfetto::protos::pbzero::TraceConfig_BuiltinDataSource;
  using ProducerConfig = ::perfetto::protos::pbzero::TraceConfig_ProducerConfig;
  using StatsdMetadata = ::perfetto::protos::pbzero::TraceConfig_StatsdMetadata;
  using GuardrailOverrides = ::perfetto::protos::pbzero::TraceConfig_GuardrailOverrides;
  using TriggerConfig = ::perfetto::protos::pbzero::TraceConfig_TriggerConfig;
  using IncrementalStateConfig = ::perfetto::protos::pbzero::TraceConfig_IncrementalStateConfig;
  using IncidentReportConfig = ::perfetto::protos::pbzero::TraceConfig_IncidentReportConfig;
  using LockdownModeOperation = ::perfetto::protos::pbzero::TraceConfig_LockdownModeOperation;
  using CompressionType = ::perfetto::protos::pbzero::TraceConfig_CompressionType;
  static const LockdownModeOperation LOCKDOWN_UNCHANGED = TraceConfig_LockdownModeOperation_LOCKDOWN_UNCHANGED;
  static const LockdownModeOperation LOCKDOWN_CLEAR = TraceConfig_LockdownModeOperation_LOCKDOWN_CLEAR;
  static const LockdownModeOperation LOCKDOWN_SET = TraceConfig_LockdownModeOperation_LOCKDOWN_SET;
  static const CompressionType COMPRESSION_TYPE_UNSPECIFIED = TraceConfig_CompressionType_COMPRESSION_TYPE_UNSPECIFIED;
  static const CompressionType COMPRESSION_TYPE_DEFLATE = TraceConfig_CompressionType_COMPRESSION_TYPE_DEFLATE;
  template <typename T = TraceConfig_BufferConfig> T* add_buffers() {
    return BeginNestedMessage<T>(1);
  }

  template <typename T = TraceConfig_DataSource> T* add_data_sources() {
    return BeginNestedMessage<T>(2);
  }

  template <typename T = TraceConfig_BuiltinDataSource> T* set_builtin_data_sources() {
    return BeginNestedMessage<T>(20);
  }

  void set_duration_ms(uint32_t value) {
    AppendVarInt(3, value);
  }
  void set_enable_extra_guardrails(bool value) {
    AppendTinyVarInt(4, value);
  }
  void set_lockdown_mode(::perfetto::protos::pbzero::TraceConfig_LockdownModeOperation value) {
    AppendTinyVarInt(5, value);
  }
  template <typename T = TraceConfig_ProducerConfig> T* add_producers() {
    return BeginNestedMessage<T>(6);
  }

  template <typename T = TraceConfig_StatsdMetadata> T* set_statsd_metadata() {
    return BeginNestedMessage<T>(7);
  }

  void set_write_into_file(bool value) {
    AppendTinyVarInt(8, value);
  }
  void set_output_path(const std::string& value) {
    AppendBytes(29, value.data(), value.size());
  }
  void set_output_path(const char* data, size_t size) {
    AppendBytes(29, data, size);
  }
  void set_file_write_period_ms(uint32_t value) {
    AppendVarInt(9, value);
  }
  void set_max_file_size_bytes(uint64_t value) {
    AppendVarInt(10, value);
  }
  template <typename T = TraceConfig_GuardrailOverrides> T* set_guardrail_overrides() {
    return BeginNestedMessage<T>(11);
  }

  void set_deferred_start(bool value) {
    AppendTinyVarInt(12, value);
  }
  void set_flush_period_ms(uint32_t value) {
    AppendVarInt(13, value);
  }
  void set_flush_timeout_ms(uint32_t value) {
    AppendVarInt(14, value);
  }
  void set_data_source_stop_timeout_ms(uint32_t value) {
    AppendVarInt(23, value);
  }
  void set_notify_traceur(bool value) {
    AppendTinyVarInt(16, value);
  }
  template <typename T = TraceConfig_TriggerConfig> T* set_trigger_config() {
    return BeginNestedMessage<T>(17);
  }

  void add_activate_triggers(const std::string& value) {
    AppendBytes(18, value.data(), value.size());
  }
  void add_activate_triggers(const char* data, size_t size) {
    AppendBytes(18, data, size);
  }
  template <typename T = TraceConfig_IncrementalStateConfig> T* set_incremental_state_config() {
    return BeginNestedMessage<T>(21);
  }

  void set_allow_user_build_tracing(bool value) {
    AppendTinyVarInt(19, value);
  }
  void set_unique_session_name(const std::string& value) {
    AppendBytes(22, value.data(), value.size());
  }
  void set_unique_session_name(const char* data, size_t size) {
    AppendBytes(22, data, size);
  }
  void set_compression_type(::perfetto::protos::pbzero::TraceConfig_CompressionType value) {
    AppendTinyVarInt(24, value);
  }
  template <typename T = TraceConfig_IncidentReportConfig> T* set_incident_report_config() {
    return BeginNestedMessage<T>(25);
  }

  void set_trace_uuid_msb(int64_t value) {
    AppendVarInt(27, value);
  }
  void set_trace_uuid_lsb(int64_t value) {
    AppendVarInt(28, value);
  }
};

class TraceConfig_IncidentReportConfig_Decoder : public ::protozero::TypedProtoDecoder</*MAX_FIELD_ID=*/4, /*HAS_NONPACKED_REPEATED_FIELDS=*/false> {
 public:
  TraceConfig_IncidentReportConfig_Decoder(const uint8_t* data, size_t len) : TypedProtoDecoder(data, len) {}
  explicit TraceConfig_IncidentReportConfig_Decoder(const std::string& raw) : TypedProtoDecoder(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()) {}
  explicit TraceConfig_IncidentReportConfig_Decoder(const ::protozero::ConstBytes& raw) : TypedProtoDecoder(raw.data, raw.size) {}
  bool has_destination_package() const { return at<1>().valid(); }
  ::protozero::ConstChars destination_package() const { return at<1>().as_string(); }
  bool has_destination_class() const { return at<2>().valid(); }
  ::protozero::ConstChars destination_class() const { return at<2>().as_string(); }
  bool has_privacy_level() const { return at<3>().valid(); }
  int32_t privacy_level() const { return at<3>().as_int32(); }
  bool has_skip_dropbox() const { return at<4>().valid(); }
  bool skip_dropbox() const { return at<4>().as_bool(); }
};

class TraceConfig_IncidentReportConfig : public ::protozero::Message {
 public:
  using Decoder = TraceConfig_IncidentReportConfig_Decoder;
  enum : int32_t {
    kDestinationPackageFieldNumber = 1,
    kDestinationClassFieldNumber = 2,
    kPrivacyLevelFieldNumber = 3,
    kSkipDropboxFieldNumber = 4,
  };
  void set_destination_package(const std::string& value) {
    AppendBytes(1, value.data(), value.size());
  }
  void set_destination_package(const char* data, size_t size) {
    AppendBytes(1, data, size);
  }
  void set_destination_class(const std::string& value) {
    AppendBytes(2, value.data(), value.size());
  }
  void set_destination_class(const char* data, size_t size) {
    AppendBytes(2, data, size);
  }
  void set_privacy_level(int32_t value) {
    AppendVarInt(3, value);
  }
  void set_skip_dropbox(bool value) {
    AppendTinyVarInt(4, value);
  }
};

class TraceConfig_IncrementalStateConfig_Decoder : public ::protozero::TypedProtoDecoder</*MAX_FIELD_ID=*/1, /*HAS_NONPACKED_REPEATED_FIELDS=*/false> {
 public:
  TraceConfig_IncrementalStateConfig_Decoder(const uint8_t* data, size_t len) : TypedProtoDecoder(data, len) {}
  explicit TraceConfig_IncrementalStateConfig_Decoder(const std::string& raw) : TypedProtoDecoder(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()) {}
  explicit TraceConfig_IncrementalStateConfig_Decoder(const ::protozero::ConstBytes& raw) : TypedProtoDecoder(raw.data, raw.size) {}
  bool has_clear_period_ms() const { return at<1>().valid(); }
  uint32_t clear_period_ms() const { return at<1>().as_uint32(); }
};

class TraceConfig_IncrementalStateConfig : public ::protozero::Message {
 public:
  using Decoder = TraceConfig_IncrementalStateConfig_Decoder;
  enum : int32_t {
    kClearPeriodMsFieldNumber = 1,
  };
  void set_clear_period_ms(uint32_t value) {
    AppendVarInt(1, value);
  }
};

class TraceConfig_TriggerConfig_Decoder : public ::protozero::TypedProtoDecoder</*MAX_FIELD_ID=*/3, /*HAS_NONPACKED_REPEATED_FIELDS=*/true> {
 public:
  TraceConfig_TriggerConfig_Decoder(const uint8_t* data, size_t len) : TypedProtoDecoder(data, len) {}
  explicit TraceConfig_TriggerConfig_Decoder(const std::string& raw) : TypedProtoDecoder(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()) {}
  explicit TraceConfig_TriggerConfig_Decoder(const ::protozero::ConstBytes& raw) : TypedProtoDecoder(raw.data, raw.size) {}
  bool has_trigger_mode() const { return at<1>().valid(); }
  int32_t trigger_mode() const { return at<1>().as_int32(); }
  bool has_triggers() const { return at<2>().valid(); }
  ::protozero::RepeatedFieldIterator<::protozero::ConstBytes> triggers() const { return GetRepeated<::protozero::ConstBytes>(2); }
  bool has_trigger_timeout_ms() const { return at<3>().valid(); }
  uint32_t trigger_timeout_ms() const { return at<3>().as_uint32(); }
};

class TraceConfig_TriggerConfig : public ::protozero::Message {
 public:
  using Decoder = TraceConfig_TriggerConfig_Decoder;
  enum : int32_t {
    kTriggerModeFieldNumber = 1,
    kTriggersFieldNumber = 2,
    kTriggerTimeoutMsFieldNumber = 3,
  };
  using Trigger = ::perfetto::protos::pbzero::TraceConfig_TriggerConfig_Trigger;
  using TriggerMode = ::perfetto::protos::pbzero::TraceConfig_TriggerConfig_TriggerMode;
  static const TriggerMode UNSPECIFIED = TraceConfig_TriggerConfig_TriggerMode_UNSPECIFIED;
  static const TriggerMode START_TRACING = TraceConfig_TriggerConfig_TriggerMode_START_TRACING;
  static const TriggerMode STOP_TRACING = TraceConfig_TriggerConfig_TriggerMode_STOP_TRACING;
  void set_trigger_mode(::perfetto::protos::pbzero::TraceConfig_TriggerConfig_TriggerMode value) {
    AppendTinyVarInt(1, value);
  }
  template <typename T = TraceConfig_TriggerConfig_Trigger> T* add_triggers() {
    return BeginNestedMessage<T>(2);
  }

  void set_trigger_timeout_ms(uint32_t value) {
    AppendVarInt(3, value);
  }
};

class TraceConfig_TriggerConfig_Trigger_Decoder : public ::protozero::TypedProtoDecoder</*MAX_FIELD_ID=*/3, /*HAS_NONPACKED_REPEATED_FIELDS=*/false> {
 public:
  TraceConfig_TriggerConfig_Trigger_Decoder(const uint8_t* data, size_t len) : TypedProtoDecoder(data, len) {}
  explicit TraceConfig_TriggerConfig_Trigger_Decoder(const std::string& raw) : TypedProtoDecoder(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()) {}
  explicit TraceConfig_TriggerConfig_Trigger_Decoder(const ::protozero::ConstBytes& raw) : TypedProtoDecoder(raw.data, raw.size) {}
  bool has_name() const { return at<1>().valid(); }
  ::protozero::ConstChars name() const { return at<1>().as_string(); }
  bool has_producer_name_regex() const { return at<2>().valid(); }
  ::protozero::ConstChars producer_name_regex() const { return at<2>().as_string(); }
  bool has_stop_delay_ms() const { return at<3>().valid(); }
  uint32_t stop_delay_ms() const { return at<3>().as_uint32(); }
};

class TraceConfig_TriggerConfig_Trigger : public ::protozero::Message {
 public:
  using Decoder = TraceConfig_TriggerConfig_Trigger_Decoder;
  enum : int32_t {
    kNameFieldNumber = 1,
    kProducerNameRegexFieldNumber = 2,
    kStopDelayMsFieldNumber = 3,
  };
  void set_name(const std::string& value) {
    AppendBytes(1, value.data(), value.size());
  }
  void set_name(const char* data, size_t size) {
    AppendBytes(1, data, size);
  }
  void set_producer_name_regex(const std::string& value) {
    AppendBytes(2, value.data(), value.size());
  }
  void set_producer_name_regex(const char* data, size_t size) {
    AppendBytes(2, data, size);
  }
  void set_stop_delay_ms(uint32_t value) {
    AppendVarInt(3, value);
  }
};

class TraceConfig_GuardrailOverrides_Decoder : public ::protozero::TypedProtoDecoder</*MAX_FIELD_ID=*/1, /*HAS_NONPACKED_REPEATED_FIELDS=*/false> {
 public:
  TraceConfig_GuardrailOverrides_Decoder(const uint8_t* data, size_t len) : TypedProtoDecoder(data, len) {}
  explicit TraceConfig_GuardrailOverrides_Decoder(const std::string& raw) : TypedProtoDecoder(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()) {}
  explicit TraceConfig_GuardrailOverrides_Decoder(const ::protozero::ConstBytes& raw) : TypedProtoDecoder(raw.data, raw.size) {}
  bool has_max_upload_per_day_bytes() const { return at<1>().valid(); }
  uint64_t max_upload_per_day_bytes() const { return at<1>().as_uint64(); }
};

class TraceConfig_GuardrailOverrides : public ::protozero::Message {
 public:
  using Decoder = TraceConfig_GuardrailOverrides_Decoder;
  enum : int32_t {
    kMaxUploadPerDayBytesFieldNumber = 1,
  };
  void set_max_upload_per_day_bytes(uint64_t value) {
    AppendVarInt(1, value);
  }
};

class TraceConfig_StatsdMetadata_Decoder : public ::protozero::TypedProtoDecoder</*MAX_FIELD_ID=*/4, /*HAS_NONPACKED_REPEATED_FIELDS=*/false> {
 public:
  TraceConfig_StatsdMetadata_Decoder(const uint8_t* data, size_t len) : TypedProtoDecoder(data, len) {}
  explicit TraceConfig_StatsdMetadata_Decoder(const std::string& raw) : TypedProtoDecoder(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()) {}
  explicit TraceConfig_StatsdMetadata_Decoder(const ::protozero::ConstBytes& raw) : TypedProtoDecoder(raw.data, raw.size) {}
  bool has_triggering_alert_id() const { return at<1>().valid(); }
  int64_t triggering_alert_id() const { return at<1>().as_int64(); }
  bool has_triggering_config_uid() const { return at<2>().valid(); }
  int32_t triggering_config_uid() const { return at<2>().as_int32(); }
  bool has_triggering_config_id() const { return at<3>().valid(); }
  int64_t triggering_config_id() const { return at<3>().as_int64(); }
  bool has_triggering_subscription_id() const { return at<4>().valid(); }
  int64_t triggering_subscription_id() const { return at<4>().as_int64(); }
};

class TraceConfig_StatsdMetadata : public ::protozero::Message {
 public:
  using Decoder = TraceConfig_StatsdMetadata_Decoder;
  enum : int32_t {
    kTriggeringAlertIdFieldNumber = 1,
    kTriggeringConfigUidFieldNumber = 2,
    kTriggeringConfigIdFieldNumber = 3,
    kTriggeringSubscriptionIdFieldNumber = 4,
  };
  void set_triggering_alert_id(int64_t value) {
    AppendVarInt(1, value);
  }
  void set_triggering_config_uid(int32_t value) {
    AppendVarInt(2, value);
  }
  void set_triggering_config_id(int64_t value) {
    AppendVarInt(3, value);
  }
  void set_triggering_subscription_id(int64_t value) {
    AppendVarInt(4, value);
  }
};

class TraceConfig_ProducerConfig_Decoder : public ::protozero::TypedProtoDecoder</*MAX_FIELD_ID=*/3, /*HAS_NONPACKED_REPEATED_FIELDS=*/false> {
 public:
  TraceConfig_ProducerConfig_Decoder(const uint8_t* data, size_t len) : TypedProtoDecoder(data, len) {}
  explicit TraceConfig_ProducerConfig_Decoder(const std::string& raw) : TypedProtoDecoder(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()) {}
  explicit TraceConfig_ProducerConfig_Decoder(const ::protozero::ConstBytes& raw) : TypedProtoDecoder(raw.data, raw.size) {}
  bool has_producer_name() const { return at<1>().valid(); }
  ::protozero::ConstChars producer_name() const { return at<1>().as_string(); }
  bool has_shm_size_kb() const { return at<2>().valid(); }
  uint32_t shm_size_kb() const { return at<2>().as_uint32(); }
  bool has_page_size_kb() const { return at<3>().valid(); }
  uint32_t page_size_kb() const { return at<3>().as_uint32(); }
};

class TraceConfig_ProducerConfig : public ::protozero::Message {
 public:
  using Decoder = TraceConfig_ProducerConfig_Decoder;
  enum : int32_t {
    kProducerNameFieldNumber = 1,
    kShmSizeKbFieldNumber = 2,
    kPageSizeKbFieldNumber = 3,
  };
  void set_producer_name(const std::string& value) {
    AppendBytes(1, value.data(), value.size());
  }
  void set_producer_name(const char* data, size_t size) {
    AppendBytes(1, data, size);
  }
  void set_shm_size_kb(uint32_t value) {
    AppendVarInt(2, value);
  }
  void set_page_size_kb(uint32_t value) {
    AppendVarInt(3, value);
  }
};

class TraceConfig_BuiltinDataSource_Decoder : public ::protozero::TypedProtoDecoder</*MAX_FIELD_ID=*/6, /*HAS_NONPACKED_REPEATED_FIELDS=*/false> {
 public:
  TraceConfig_BuiltinDataSource_Decoder(const uint8_t* data, size_t len) : TypedProtoDecoder(data, len) {}
  explicit TraceConfig_BuiltinDataSource_Decoder(const std::string& raw) : TypedProtoDecoder(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()) {}
  explicit TraceConfig_BuiltinDataSource_Decoder(const ::protozero::ConstBytes& raw) : TypedProtoDecoder(raw.data, raw.size) {}
  bool has_disable_clock_snapshotting() const { return at<1>().valid(); }
  bool disable_clock_snapshotting() const { return at<1>().as_bool(); }
  bool has_disable_trace_config() const { return at<2>().valid(); }
  bool disable_trace_config() const { return at<2>().as_bool(); }
  bool has_disable_system_info() const { return at<3>().valid(); }
  bool disable_system_info() const { return at<3>().as_bool(); }
  bool has_disable_service_events() const { return at<4>().valid(); }
  bool disable_service_events() const { return at<4>().as_bool(); }
  bool has_primary_trace_clock() const { return at<5>().valid(); }
  int32_t primary_trace_clock() const { return at<5>().as_int32(); }
  bool has_snapshot_interval_ms() const { return at<6>().valid(); }
  uint32_t snapshot_interval_ms() const { return at<6>().as_uint32(); }
};

class TraceConfig_BuiltinDataSource : public ::protozero::Message {
 public:
  using Decoder = TraceConfig_BuiltinDataSource_Decoder;
  enum : int32_t {
    kDisableClockSnapshottingFieldNumber = 1,
    kDisableTraceConfigFieldNumber = 2,
    kDisableSystemInfoFieldNumber = 3,
    kDisableServiceEventsFieldNumber = 4,
    kPrimaryTraceClockFieldNumber = 5,
    kSnapshotIntervalMsFieldNumber = 6,
  };
  void set_disable_clock_snapshotting(bool value) {
    AppendTinyVarInt(1, value);
  }
  void set_disable_trace_config(bool value) {
    AppendTinyVarInt(2, value);
  }
  void set_disable_system_info(bool value) {
    AppendTinyVarInt(3, value);
  }
  void set_disable_service_events(bool value) {
    AppendTinyVarInt(4, value);
  }
  void set_primary_trace_clock(::perfetto::protos::pbzero::BuiltinClock value) {
    AppendTinyVarInt(5, value);
  }
  void set_snapshot_interval_ms(uint32_t value) {
    AppendVarInt(6, value);
  }
};

class TraceConfig_DataSource_Decoder : public ::protozero::TypedProtoDecoder</*MAX_FIELD_ID=*/3, /*HAS_NONPACKED_REPEATED_FIELDS=*/true> {
 public:
  TraceConfig_DataSource_Decoder(const uint8_t* data, size_t len) : TypedProtoDecoder(data, len) {}
  explicit TraceConfig_DataSource_Decoder(const std::string& raw) : TypedProtoDecoder(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()) {}
  explicit TraceConfig_DataSource_Decoder(const ::protozero::ConstBytes& raw) : TypedProtoDecoder(raw.data, raw.size) {}
  bool has_config() const { return at<1>().valid(); }
  ::protozero::ConstBytes config() const { return at<1>().as_bytes(); }
  bool has_producer_name_filter() const { return at<2>().valid(); }
  ::protozero::RepeatedFieldIterator<::protozero::ConstChars> producer_name_filter() const { return GetRepeated<::protozero::ConstChars>(2); }
  bool has_producer_name_regex_filter() const { return at<3>().valid(); }
  ::protozero::RepeatedFieldIterator<::protozero::ConstChars> producer_name_regex_filter() const { return GetRepeated<::protozero::ConstChars>(3); }
};

class TraceConfig_DataSource : public ::protozero::Message {
 public:
  using Decoder = TraceConfig_DataSource_Decoder;
  enum : int32_t {
    kConfigFieldNumber = 1,
    kProducerNameFilterFieldNumber = 2,
    kProducerNameRegexFilterFieldNumber = 3,
  };
  template <typename T = DataSourceConfig> T* set_config() {
    return BeginNestedMessage<T>(1);
  }

  void add_producer_name_filter(const std::string& value) {
    AppendBytes(2, value.data(), value.size());
  }
  void add_producer_name_filter(const char* data, size_t size) {
    AppendBytes(2, data, size);
  }
  void add_producer_name_regex_filter(const std::string& value) {
    AppendBytes(3, value.data(), value.size());
  }
  void add_producer_name_regex_filter(const char* data, size_t size) {
    AppendBytes(3, data, size);
  }
};

class TraceConfig_BufferConfig_Decoder : public ::protozero::TypedProtoDecoder</*MAX_FIELD_ID=*/4, /*HAS_NONPACKED_REPEATED_FIELDS=*/false> {
 public:
  TraceConfig_BufferConfig_Decoder(const uint8_t* data, size_t len) : TypedProtoDecoder(data, len) {}
  explicit TraceConfig_BufferConfig_Decoder(const std::string& raw) : TypedProtoDecoder(reinterpret_cast<const uint8_t*>(raw.data()), raw.size()) {}
  explicit TraceConfig_BufferConfig_Decoder(const ::protozero::ConstBytes& raw) : TypedProtoDecoder(raw.data, raw.size) {}
  bool has_size_kb() const { return at<1>().valid(); }
  uint32_t size_kb() const { return at<1>().as_uint32(); }
  bool has_fill_policy() const { return at<4>().valid(); }
  int32_t fill_policy() const { return at<4>().as_int32(); }
};

class TraceConfig_BufferConfig : public ::protozero::Message {
 public:
  using Decoder = TraceConfig_BufferConfig_Decoder;
  enum : int32_t {
    kSizeKbFieldNumber = 1,
    kFillPolicyFieldNumber = 4,
  };
  using FillPolicy = ::perfetto::protos::pbzero::TraceConfig_BufferConfig_FillPolicy;
  static const FillPolicy UNSPECIFIED = TraceConfig_BufferConfig_FillPolicy_UNSPECIFIED;
  static const FillPolicy RING_BUFFER = TraceConfig_BufferConfig_FillPolicy_RING_BUFFER;
  static const FillPolicy DISCARD = TraceConfig_BufferConfig_FillPolicy_DISCARD;
  void set_size_kb(uint32_t value) {
    AppendVarInt(1, value);
  }
  void set_fill_policy(::perfetto::protos::pbzero::TraceConfig_BufferConfig_FillPolicy value) {
    AppendTinyVarInt(4, value);
  }
};

} // Namespace.
} // Namespace.
} // Namespace.
#endif  // Include guard.
