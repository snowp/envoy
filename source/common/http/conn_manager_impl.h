#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "envoy/access_log/access_log.h"
#include "envoy/common/scope_tracker.h"
#include "envoy/event/deferred_deletable.h"
#include "envoy/http/api_listener.h"
#include "envoy/http/codec.h"
#include "envoy/http/codes.h"
#include "envoy/http/context.h"
#include "envoy/http/filter.h"
#include "envoy/http/header_map.h"
#include "envoy/network/connection.h"
#include "envoy/network/drain_decision.h"
#include "envoy/network/filter.h"
#include "envoy/router/rds.h"
#include "envoy/router/router.h"
#include "envoy/router/scopes.h"
#include "envoy/runtime/runtime.h"
#include "envoy/server/overload_manager.h"
#include "envoy/ssl/connection.h"
#include "envoy/stats/scope.h"
#include "envoy/stats/stats_macros.h"
#include "envoy/tracing/http_tracer.h"
#include "envoy/upstream/upstream.h"

#include "common/buffer/watermark_buffer.h"
#include "common/common/dump_state_utils.h"
#include "common/common/linked_object.h"
#include "common/grpc/common.h"
#include "common/http/conn_manager_config.h"
#include "common/http/filter_manager.h"
#include "common/http/user_agent.h"
#include "common/http/utility.h"
#include "common/stream_info/stream_info_impl.h"
#include "common/tracing/http_tracer_impl.h"

namespace Envoy {
namespace Http {

/**
 * Implementation of both ConnectionManager and ServerConnectionCallbacks. This is a
 * Network::Filter that can be installed on a connection that will perform HTTP protocol agnostic
 * handling of a connection and all requests/pushes that occur on a connection.
 */
class ConnectionManagerImpl : Logger::Loggable<Logger::Id::http>,
                              public Network::ReadFilter,
                              public ServerConnectionCallbacks,
                              public Network::ConnectionCallbacks,
                              public Http::ApiListener {
public:
  ConnectionManagerImpl(ConnectionManagerConfig& config, const Network::DrainDecision& drain_close,
                        Runtime::RandomGenerator& random_generator, Http::Context& http_context,
                        Runtime::Loader& runtime, const LocalInfo::LocalInfo& local_info,
                        Upstream::ClusterManager& cluster_manager,
                        Server::OverloadManager* overload_manager, TimeSource& time_system);
  ~ConnectionManagerImpl() override;

  static ConnectionManagerStats generateStats(const std::string& prefix, Stats::Scope& scope);
  static ConnectionManagerTracingStats generateTracingStats(const std::string& prefix,
                                                            Stats::Scope& scope);
  static void chargeTracingStats(const Tracing::Reason& tracing_reason,
                                 ConnectionManagerTracingStats& tracing_stats);
  static ConnectionManagerListenerStats generateListenerStats(const std::string& prefix,
                                                              Stats::Scope& scope);
  static const ResponseHeaderMap& continueHeader();

  // Currently the ConnectionManager creates a codec lazily when either:
  //   a) onConnection for H3.
  //   b) onData for H1 and H2.
  // With the introduction of ApiListeners, neither event occurs. This function allows consumer code
  // to manually create a codec.
  // TODO(junr03): consider passing a synthetic codec instead of creating once. The codec in the
  // ApiListener case is solely used to determine the protocol version.
  void createCodec(Buffer::Instance& data);

  // Network::ReadFilter
  Network::FilterStatus onData(Buffer::Instance& data, bool end_stream) override;
  Network::FilterStatus onNewConnection() override;
  void initializeReadFilterCallbacks(Network::ReadFilterCallbacks& callbacks) override;

  // Http::ConnectionCallbacks
  void onGoAway() override;

  // Http::ServerConnectionCallbacks
  RequestDecoder& newStream(ResponseEncoder& response_encoder, bool is_internally_created = false);

  // Network::ConnectionCallbacks
  void onEvent(Network::ConnectionEvent event) override;
  // Pass connection watermark events on to all the streams associated with that connection.
  void onAboveWriteBufferHighWatermark() override {
    codec_->onUnderlyingConnectionAboveWriteBufferHighWatermark();
  }
  void onBelowWriteBufferLowWatermark() override {
    codec_->onUnderlyingConnectionBelowWriteBufferLowWatermark();
  }

  TimeSource& timeSource() { return time_source_; }

  // Return a reference to the shared_ptr so that it can be lazy created on demand.
  std::shared_ptr<StreamInfo::FilterState>& filterState() { return filter_state_; }

private:
  struct ActiveStream;

  class RdsRouteConfigUpdateRequester : public RouteConfigUpdateRequester {
  public:
    RdsRouteConfigUpdateRequester(Router::RouteConfigProvider* route_config_provider)
        : route_config_provider_(route_config_provider) {}
    void requestRouteConfigUpdate(
        const std::string host_header, Event::Dispatcher& thread_local_dispatcher,
        Http::RouteConfigUpdatedCallbackSharedPtr route_config_updated_cb) override;

  private:
    Router::RouteConfigProvider* route_config_provider_;
  };

  class NullRouteConfigUpdateRequester : public RouteConfigUpdateRequester {
  public:
    NullRouteConfigUpdateRequester() = default;
  };

  static std::unique_ptr<RouteConfigUpdateRequester>
  routeConfigUpdateRequester(ConnectionManagerConfig& config);
  /**
   * Wraps a single active stream on the connection. These are either full request/response pairs
   * or pushes.
   */
  struct ActiveStream : LinkedObject<ActiveStream>,
                        public Event::DeferredDeletable,
                        public StreamCallbacks,
                        public RequestDecoder,
                        public FilterManagerCallbacks,
                        public Tracing::Config {
    ActiveStream(ConnectionManagerImpl& connection_manager);
    ~ActiveStream() override;

    // Indicates which filter to start the iteration with.
    enum class FilterIterationStartState { AlwaysStartFromNext, CanStartFromCurrent };

    void chargeStats(const ResponseHeaderMap& headers);
    const Network::Connection* connection();

    // This is a helper function for encodeHeaders and responseDataTooLarge which allows for shared
    // code for the two headers encoding paths. It does header munging, updates timing stats, and
    // sends the headers to the encoder.
    void encodeHeadersInternal(ResponseHeaderMap& headers, bool end_stream);
    // This is a helper function for encodeData and responseDataTooLarge which allows for shared
    // code for the two data encoding paths. It does stats updates and tracks potential end of
    // stream.
    void encodeDataInternal(Buffer::Instance& data, bool end_stream);

    // FilterManagerCallbacks
    void encodeFiltered100ContinueHeaders(const Http::RequestHeaderMap& request_headers,
                                          Http::ResponseHeaderMap& response_headers) override;
    void encodeFilteredHeaders(Http::ResponseHeaderMap& headers, bool end_stream) override {
      encodeHeadersInternal(headers, end_stream);
    }
    void encodeFilteredMetadata(MetadataMapVector&& metadata) override {
      response_encoder_->encodeMetadata(metadata);
    }
    void encodeFilteredData(Buffer::Instance&& data, bool end_stream) override {
      encodeDataInternal(data, end_stream);
    }
    void encodeFilteredData(Buffer::Instance& data, bool end_stream) override {
      encodeDataInternal(data, end_stream);
    }
    void encodeFilteredTrailers(ResponseTrailerMap& trailers) override {
      response_encoder_->encodeTrailers(trailers);
    }
    void onUpgrade() override {
      connection_manager_.stats_.named_.downstream_cx_upgrades_total_.inc();
      connection_manager_.stats_.named_.downstream_cx_upgrades_active_.inc();
    }
    void decUpgrade() override {
      connection_manager_.stats_.named_.downstream_cx_upgrades_active_.dec();
    }
    void onLocalResetStream() override {
      connection_manager_.stats_.named_.downstream_rq_tx_reset_.inc();
      connection_manager_.doEndStream(*this);
    }
    void resetStream() override {
      // TODO
    }
    void requestTooLarge() override {
      connection_manager_.stats_.named_.downstream_rq_too_large_.inc();
    }
    void responseDataTooLarge() override { connection_manager_.stats_.named_.rs_too_large_.inc(); }
    void decoderAboveWriteBufferHighWatermark() override {
      response_encoder_->getStream().readDisable(true);
      connection_manager_.stats_.named_.downstream_flow_control_paused_reading_total_.inc();
    }
    void decoderBelowWriteBufferLowWatermark() override {
      response_encoder_->getStream().readDisable(false);
      connection_manager_.stats_.named_.downstream_flow_control_resumed_reading_total_.inc();
    }
    void onIdleTimeout() override {
      connection_manager_.stats_.named_.downstream_rq_idle_timeout_.inc();
    }
    void onRequestTimeout() override {
      connection_manager_.stats_.named_.downstream_rq_timeout_.inc();
    }
    void onStreamMaxDurationReached() override {
      connection_manager_.stats_.named_.downstream_rq_max_duration_reached_.inc();
    }
    Router::RouteConstSharedPtr evaluateRoute(const Http::RequestHeaderMap& headers,
                                              const StreamInfo::StreamInfo& stream_info) override {
      Router::RouteConstSharedPtr route;
      if (connection_manager_.config_.isRoutable() &&
          connection_manager_.config_.scopedRouteConfigProvider() != nullptr) {
        // NOTE: re-select scope as well in case the scope key header has been changed by a filter.
        snapScopedRouteConfig(headers);
      }
      if (snapped_route_config_ != nullptr) {
        return snapped_route_config_->route(headers, stream_info, stream_id_);
      }

      return nullptr;
    }

    void evaluateCustomTags(Router::RouteConstSharedPtr route) override {
      if (!connection_manager_.config_.tracingConfig()) {
        return;
      }
      const Tracing::CustomTagMap& conn_manager_tags =
          connection_manager_.config_.tracingConfig()->custom_tags_;
      const Tracing::CustomTagMap* route_tags = nullptr;
      if (route && route->tracingConfig()) {
        route_tags = &route->tracingConfig()->getCustomTags();
      }
      const bool configured_in_conn = !conn_manager_tags.empty();
      const bool configured_in_route = route_tags && !route_tags->empty();
      if (!configured_in_conn && !configured_in_route) {
        return;
      }
      Tracing::CustomTagMap& custom_tag_map = getOrMakeTracingCustomTagMap();
      if (configured_in_route) {
        custom_tag_map.insert(route_tags->begin(), route_tags->end());
      }
      if (configured_in_conn) {
        custom_tag_map.insert(conn_manager_tags.begin(), conn_manager_tags.end());
      }
    }
    void clearCustomTags() override {
      if (tracing_custom_tags_) {
        tracing_custom_tags_->clear();
      }
    }

    void endStream() override { connection_manager_.doEndStream(*this); }

    Tracing::CustomTagMap& getOrMakeTracingCustomTagMap() {
      if (tracing_custom_tags_ == nullptr) {
        tracing_custom_tags_ = std::make_unique<Tracing::CustomTagMap>();
      }
      return *tracing_custom_tags_;
    }

    RequestHeaderMapPtr newStream(RequestHeaderMapPtr&& request_headers) override {
      // n.b. we do not currently change the codecs to point at the new stream
      // decoder because the decoder callbacks are complete. It would be good to
      // null out that pointer but should not be necessary.
      ResponseEncoder* response_encoder = response_encoder_;
      response_encoder_ = nullptr;
      response_encoder->getStream().removeCallbacks(*this);
      // This functionally deletes the stream (via deferred delete) so do not
      // reference anything beyond this point.
      connection_manager_.doEndStream(*this);

      RequestDecoder& new_stream = connection_manager_.newStream(*response_encoder, true);
      // We don't need to copy over the old parent FilterState from the old StreamInfo if it did not
      // store any objects with a LifeSpan at or above DownstreamRequest. This is to avoid
      // unnecessary heap allocation.
      if (filter_manager_.stream_info_.filter_state_->hasDataAtOrAboveLifeSpan(
              StreamInfo::FilterState::LifeSpan::DownstreamRequest)) {
        (*connection_manager_.streams_.begin())->filter_manager_.stream_info_.filter_state_ =
            std::make_shared<StreamInfo::FilterStateImpl>(
                filter_manager_.stream_info_.filter_state_->parent(),
                StreamInfo::FilterState::LifeSpan::FilterChain);
      }

      new_stream.decodeHeaders(std::move(request_headers), true);
      return nullptr;
    }
    Http1StreamEncoderOptionsOptRef http1StreamEncoderOptions() override {
      return response_encoder_->http1StreamEncoderOptions();
    }

    // Http::StreamCallbacks
    void onResetStream(StreamResetReason reason,
                       absl::string_view transport_failure_reason) override;
    void onAboveWriteBufferHighWatermark() override;
    void onBelowWriteBufferLowWatermark() override;

    // Http::StreamDecoder
    void decodeData(Buffer::Instance& data, bool end_stream) override;
    void decodeMetadata(MetadataMapPtr&&) override;

    // Http::RequestDecoder
    void decodeHeaders(RequestHeaderMapPtr&& headers, bool end_stream) override;
    void decodeTrailers(RequestTrailerMapPtr&& trailers) override;

    // Tracing::TracingConfig
    Tracing::OperationName operationName() const override;
    const Tracing::CustomTagMap* customTags() const override;
    bool verbose() const override;
    uint32_t maxPathTagLength() const override;

    Tracing::SpanPtr traceRequest(RequestHeaderMap& request_headers);

    Router::ConfigConstSharedPtr scopedRouteConfig(const RequestHeaderMap& request_headers);
    absl::optional<Router::ConfigConstSharedPtr> getRouteConfig() override;
    // Updates the snapped_route_config_ (by reselecting scoped route configuration), if a scope is
    // not found, snapped_route_config_ is set to Router::NullConfigImpl.
    void snapScopedRouteConfig(const RequestHeaderMap& request_headers);
    void
    requestRouteConfigUpdate(Event::Dispatcher& thread_local_dispatcher,
                             Http::RouteConfigUpdatedCallbackSharedPtr route_config_updated_cb);

    friend std::ostream& operator<<(std::ostream& os, const ActiveStream& s) {
      s.filter_manager_.dumpState(os);
      return os;
    }

    ConnectionManagerImpl& connection_manager_;
    const uint64_t stream_id_;
    FilterManager filter_manager_;
    ResponseEncoder* response_encoder_{};
    Stats::TimespanPtr request_response_timespan_;
    Router::ConfigConstSharedPtr snapped_route_config_;
    Router::ScopedConfigConstSharedPtr snapped_scoped_routes_config_;
    std::unique_ptr<Tracing::CustomTagMap> tracing_custom_tags_{nullptr};
    // StreamInfo::StreamInfoImpl stream_info_;
    const std::string* decorated_operation_{nullptr};

    // True if this stream is internally created. Currently only used for
    // internal redirects or other streams created via recreateStream().
    bool is_internally_created_ : 1;
    bool saw_connection_close_ : 1;
    bool decorated_propagate_ : 1;
  };

  using ActiveStreamPtr = std::unique_ptr<ActiveStream>;

  /**
   * Check to see if the connection can be closed after gracefully waiting to send pending codec
   * data.
   */
  void checkForDeferredClose();

  /**
   * Do a delayed destruction of a stream to allow for stack unwind. Also calls onDestroy() for
   * each filter.
   */
  void doDeferredStreamDestroy(ActiveStream& stream);

  /**
   * Process a stream that is ending due to upstream response or reset.
   */
  void doEndStream(ActiveStream& stream);

  void resetAllStreams(absl::optional<StreamInfo::ResponseFlag> response_flag);
  void onIdleTimeout();
  void onConnectionDurationTimeout();
  void onDrainTimeout();
  void startDrainSequence();
  Tracing::HttpTracer& tracer() { return *config_.tracer(); }
  void handleCodecException(const char* error);
  void doConnectionClose(absl::optional<Network::ConnectionCloseType> close_type,
                         absl::optional<StreamInfo::ResponseFlag> response_flag);

  enum class DrainState { NotDraining, Draining, Closing };

  ConnectionManagerConfig& config_;
  ConnectionManagerStats& stats_; // We store a reference here to avoid an extra stats() call on the
                                  // config in the hot path.
  ServerConnectionPtr codec_;
  std::list<ActiveStreamPtr> streams_;
  Stats::TimespanPtr conn_length_;
  const Network::DrainDecision& drain_close_;
  DrainState drain_state_{DrainState::NotDraining};
  UserAgent user_agent_;
  // An idle timer for the connection. This is only armed when there are no streams on the
  // connection. When there are active streams it is disarmed in favor of each stream's
  // stream_idle_timer_.
  Event::TimerPtr connection_idle_timer_;
  // A connection duration timer. Armed during handling new connection if enabled in config.
  Event::TimerPtr connection_duration_timer_;
  Event::TimerPtr drain_timer_;
  Runtime::RandomGenerator& random_generator_;
  Http::Context& http_context_;
  Runtime::Loader& runtime_;
  const LocalInfo::LocalInfo& local_info_;
  Upstream::ClusterManager& cluster_manager_;
  Network::ReadFilterCallbacks* read_callbacks_{};
  ConnectionManagerListenerStats& listener_stats_;
  // References into the overload manager thread local state map. Using these lets us avoid a map
  // lookup in the hot path of processing each request.
  const Server::OverloadActionState& overload_stop_accepting_requests_ref_;
  const Server::OverloadActionState& overload_disable_keepalive_ref_;
  TimeSource& time_source_;
  std::shared_ptr<StreamInfo::FilterState> filter_state_;
};

} // namespace Http
} // namespace Envoy
