#include "extensions/filters/http/http1_grpc_bridge/http1_bridge_filter.h"

#include <cstdint>
#include <string>
#include <vector>

#include "envoy/http/codes.h"

#include "common/common/enum_to_int.h"
#include "common/common/utility.h"
#include "common/grpc/common.h"
#include "common/http/filter_utility.h"
#include "common/http/headers.h"
#include "common/http/http1/codec_impl.h"

#include <arpa/inet.h>

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Http1GrpcBridge {


void prependMessage(Buffer::Instance& buffer, uint32_t size) {
  Buffer::OwnedImpl tmp;
  tmp.add("\0", 1);
  auto nl_size = htonl(size);
  tmp.add(&nl_size, 4);
  tmp.move(buffer);
  buffer.move(tmp);
}

void Http1BridgeFilter::chargeStat(const Http::HeaderMap& headers) {
  Grpc::Common::chargeStat(*cluster_, "grpc", grpc_service_, grpc_method_, headers.GrpcStatus());
}

static Http::LowerCaseString response_size("x-envoy-msg-response-size");

Http::FilterHeadersStatus Http1BridgeFilter::decodeHeaders(Http::HeaderMap& headers, bool) {
  const Http::HeaderEntry* content_type = headers.ContentType();
  const bool grpc_request =
      content_type && content_type->value() == Http::Headers::get().ContentTypeValues.Grpc.c_str();
  if (grpc_request) {
    setupStatTracking(headers);
  }

  const absl::optional<Http::Protocol>& protocol = decoder_callbacks_->requestInfo().protocol();
  ASSERT(protocol);
  if (protocol.value() == Http::Protocol::Http2 && grpc_request) {
    // we now expect an octet stream of protobuf
    headers.ContentType()->value(std::string("application/x-protobuf"));
    headers.addCopy(Http::HeaderValues().Accept, std::string("application/x-protobuf"));
    // append services because square
    auto& path = headers.Path()->value();
    headers.Path()->value(std::string("/services") + std::string(path.getStringView()));
    do_bridging_ = true;
    data_drained_ = false;
  }

  return Http::FilterHeadersStatus::Continue;
}


Http::FilterDataStatus Http1BridgeFilter::decodeData(Buffer::Instance& data, bool) {
  // Remove the first 5 bytes of the downstream request, corresponding to the compression
  // byte and the 4 uint32 message length bytes
  if (!data_drained_) {
    data.drain(5);
    data_drained_ = true;
  }
  return Http::FilterDataStatus::Continue;
}

Http::FilterHeadersStatus Http1BridgeFilter::encodeHeaders(Http::HeaderMap& headers, bool) {
  if (do_stat_tracking_) {
    chargeStat(headers);
  }

  if (do_bridging_) {
    // Since we're decoding an H/1.1 request, trailers should be empty. We insert a status code
    // based on the response status code.
    // Here we check for grpc-status. If it's not zero, we change the response code. We assume
    // that if a reset comes in and we disconnect the HTTP/1.1 client it will raise some type
    // of exception/error that the response was not complete.

    // convert content-type back to application/grpc
    if (headers.ContentType()) { headers.ContentType()->value(std::string("application/grpc")); }

    auto& status = headers.Status()->value();
    if (status.getStringView() == "200") {
      encoder_callbacks_->addEncodedTrailers().insertGrpcStatus().value(std::string("0")); 
    } else {
      encoder_callbacks_->addEncodedTrailers().insertGrpcStatus().value(std::string("2"));
    }

  }

  return Http::FilterHeadersStatus::Continue;
}
Http::FilterDataStatus Http1BridgeFilter::encodeData(Buffer::Instance& data, bool end_stream) {
  // If we reach this point we don't have knowledge of the message
  // size, and so must buffer the entire response in order to count
  // the number of bytes.
  if (!end_stream) {
    buffer_.move(data); 
    return Http::FilterDataStatus::StopIterationAndBuffer;
  } else {
    // Since we're at the end of the stream, we can now determine
    // the size of the buffer and prepend the Message-Size block.
    uint32_t length = buffer_.length();

    prependMessage(buffer_, length);
    data.move(buffer_);
    data_written_ = true;
    return Http::FilterDataStatus::Continue;
  }
}

Http::FilterTrailersStatus Http1BridgeFilter::encodeTrailers(Http::HeaderMap& trailers) {
  if (do_stat_tracking_) {
    chargeStat(trailers);
  }

  return Http::FilterTrailersStatus::Continue;
}

void Http1BridgeFilter::setupStatTracking(const Http::HeaderMap& headers) {
  cluster_ = Http::FilterUtility::resolveClusterInfo(decoder_callbacks_, cm_);
  if (!cluster_) {
    return;
  }
  do_stat_tracking_ =
      Grpc::Common::resolveServiceAndMethod(headers.Path(), &grpc_service_, &grpc_method_);
}

} // namespace GrpcHttp1Bridge
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
