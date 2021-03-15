#include "common/http/http2/conn_pool.h"

#include <cstdint>

#include "envoy/event/dispatcher.h"
#include "envoy/upstream/upstream.h"

#include "common/http/http2/codec_impl.h"
#include "common/runtime/runtime_features.h"

namespace Envoy {
namespace Http {

// All streams are 2^31. Client streams are half that, minus stream 0. Just to be on the safe
// side we do 2^29.
static const uint64_t DEFAULT_MAX_STREAMS = (1 << 29);

void MultiplexedActiveClientBase::onGoAway(Http::GoAwayErrorCode) {
  ENVOY_CONN_LOG(debug, "remote goaway", *codec_client_);
  parent_.host()->cluster().stats().upstream_cx_close_notify_.inc();
  if (state_ != ActiveClient::State::DRAINING) {
    if (codec_client_->numActiveRequests() == 0) {
      codec_client_->close();
    } else {
      parent_.transitionActiveClientState(*this, ActiveClient::State::DRAINING);
    }
  }
}

// Adjust the concurrent stream limit if the negotiated concurrent stream limit
// is lower than the local max configured streams.
//
// Note: if multiple streams are assigned to a connection before the settings
// are received, they may still be reset by the peer. This could be avoided by
// not considering http/2 connections connected until the SETTINGS frame is
// received, but that would result in a latency penalty instead.
void MultiplexedActiveClientBase::onSettings(ReceivedSettings& settings) {
  if (Runtime::runtimeFeatureEnabled("envoy.reloadable_features.improved_stream_limit_handling") &&
      settings.maxConcurrentStreams().has_value() &&
      settings.maxConcurrentStreams().value() < concurrent_stream_limit_) {
    int64_t old_unused_capacity = currentUnusedCapacity();
    // Given config limits old_unused_capacity should never exceed int32_t.
    // TODO(alyssawilk) move remaining_streams_, concurrent_stream_limit_ and
    // currentUnusedCapacity() to be explicit int32_t
    ASSERT(std::numeric_limits<int32_t>::max() >= old_unused_capacity);
    concurrent_stream_limit_ = settings.maxConcurrentStreams().value();
    int64_t delta = old_unused_capacity - currentUnusedCapacity();
    parent_.decrClusterStreamCapacity(delta);
    ENVOY_CONN_LOG(trace, "Decreasing stream capacity by {}", *codec_client_, delta);
    negative_capacity_ += delta;
  }
  // As we don't increase stream limits when maxConcurrentStreams goes up, treat
  // a stream limit of 0 as a GOAWAY.
  if (concurrent_stream_limit_ == 0) {
    parent_.transitionActiveClientState(*this, ActiveClient::State::DRAINING);
  }
}

void MultiplexedActiveClientBase::onStreamDestroy() {
  parent().onStreamClosed(*this, false);

  // If we are destroying this stream because of a disconnect, do not check for drain here. We will
  // wait until the connection has been fully drained of streams and then check in the connection
  // event callback.
  if (!closed_with_active_rq_) {
    parent().checkForDrained();
  }
}

void MultiplexedActiveClientBase::onStreamReset(Http::StreamResetReason reason) {
  switch (reason) {
  case StreamResetReason::ConnectionTermination:
  case StreamResetReason::ConnectionFailure:
    parent_.host()->cluster().stats().upstream_rq_pending_failure_eject_.inc();
    closed_with_active_rq_ = true;
    break;
  case StreamResetReason::LocalReset:
  case StreamResetReason::ProtocolError:
    parent_.host()->cluster().stats().upstream_rq_tx_reset_.inc();
    break;
  case StreamResetReason::RemoteReset:
    parent_.host()->cluster().stats().upstream_rq_rx_reset_.inc();
    break;
  case StreamResetReason::LocalRefusedStreamReset:
  case StreamResetReason::RemoteRefusedStreamReset:
  case StreamResetReason::Overflow:
  case StreamResetReason::ConnectError:
    break;
  }
}

uint64_t maxStreamsPerConnection(uint64_t max_streams_config) {
  return (max_streams_config != 0) ? max_streams_config : DEFAULT_MAX_STREAMS;
}

MultiplexedActiveClientBase::MultiplexedActiveClientBase(HttpConnPoolImplBase& parent,
                                                         Stats::Counter& cx_total)
    : Envoy::Http::ActiveClient(
          parent, maxStreamsPerConnection(parent.host()->cluster().maxRequestsPerConnection()),
          parent.host()->cluster().http2Options().max_concurrent_streams().value()) {
  codec_client_->setCodecClientCallbacks(*this);
  codec_client_->setCodecConnectionCallbacks(*this);
  cx_total.inc();
}

MultiplexedActiveClientBase::MultiplexedActiveClientBase(HttpConnPoolImplBase& parent,
                                                         Stats::Counter& cx_total,
                                                         Upstream::Host::CreateConnectionData& data)
    : Envoy::Http::ActiveClient(
          parent, maxStreamsPerConnection(parent.host()->cluster().maxRequestsPerConnection()),
          parent.host()->cluster().http2Options().max_concurrent_streams().value(), data) {
  codec_client_->setCodecClientCallbacks(*this);
  codec_client_->setCodecConnectionCallbacks(*this);
  cx_total.inc();
}

MultiplexedActiveClientBase::MultiplexedActiveClientBase(Envoy::Http::HttpConnPoolImplBase& parent,
                                                         Upstream::Host::CreateConnectionData& data,
                                                         Stats::Counter& cx_total)
    : Envoy::Http::ActiveClient(
          parent, maxStreamsPerConnection(parent.host()->cluster().maxRequestsPerConnection()),
          parent.host()->cluster().http2Options().max_concurrent_streams().value(), data) {
  codec_client_->setCodecClientCallbacks(*this);
  codec_client_->setCodecConnectionCallbacks(*this);
  cx_total.inc();
}

bool MultiplexedActiveClientBase::closingWithIncompleteStream() const {
  return closed_with_active_rq_;
}

RequestEncoder& MultiplexedActiveClientBase::newStreamEncoder(ResponseDecoder& response_decoder) {
  return codec_client_->newStream(response_decoder);
}

namespace Http2 {
ActiveClient::ActiveClient(HttpConnPoolImplBase& parent)
    : MultiplexedActiveClientBase(parent,
                                  parent.host()->cluster().stats().upstream_cx_http2_total_) {}

ActiveClient::ActiveClient(Envoy::Http::HttpConnPoolImplBase& parent,
                           Upstream::Host::CreateConnectionData& data)
    : MultiplexedActiveClientBase(parent, data,
                                  parent.host()->cluster().stats().upstream_cx_http2_total_) {}

ConnectionPool::InstancePtr
allocateConnPool(Event::Dispatcher& dispatcher, Random::RandomGenerator& random_generator,
                 Upstream::HostConstSharedPtr host, Upstream::ResourcePriority priority,
                 const Network::ConnectionSocket::OptionsSharedPtr& options,
                 const Network::TransportSocketOptionsSharedPtr& transport_socket_options,
                 Upstream::ClusterConnectivityState& state) {
  return std::make_unique<FixedHttpConnPoolImpl>(
      host, priority, dispatcher, options, transport_socket_options, random_generator, state,
      [](HttpConnPoolImplBase* pool) { return std::make_unique<ActiveClient>(*pool); },
      [](Upstream::Host::CreateConnectionData& data, HttpConnPoolImplBase* pool) {
        CodecClientPtr codec{new CodecClientProd(
            CodecClient::Type::HTTP2, std::move(data.connection_), data.host_description_,
            pool->dispatcher(), pool->randomGenerator())};
        return codec;
      },
      std::vector<Protocol>{Protocol::Http2});
}

} // namespace Http2
} // namespace Http
} // namespace Envoy
