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

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace Http1GrpcBridge {

void Http1BridgeFilter::chargeStat(const Http::HeaderMap& headers) {
  Grpc::Common::chargeStat(*cluster_, "grpc", grpc_service_, grpc_method_, headers.GrpcStatus());
}

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
  }

  return Http::FilterHeadersStatus::Continue;
}

static Http::LowerCaseString traceProtoBin("trace-proto-bin"); 

Http::FilterHeadersStatus Http1BridgeFilter::encodeHeaders(Http::HeaderMap& headers, bool) {

  std::cout << "endcode headers" << std::endl;
  if (do_stat_tracking_) {
    chargeStat(headers);
  }

  if (do_bridging_) {
  std::cout << "inserting trailers" << std::endl;
    // Since we're decoding an H/1.1 request, trailers should be empty. We insert a status code
    // based on the response status code.
    // Here we check for grpc-status. If it's not zero, we change the response code. We assume
    // that if a reset comes in and we disconnect the HTTP/1.1 client it will raise some type
    // of exception/error that the response was not complete.

    // change us back to grpc
    headers.ContentType()->value(std::string("application/grpc"));
    auto& status = headers.Status()->value();
    if (status.getStringView() == "200") {
      encoder_callbacks_->addEncodedTrailers([](Http::HeaderMap& trailers) {
          trailers.insertGrpcStatus().value(std::string("0")); 
          trailers.addCopy(traceProtoBin, std::string("jher831yy13JHy3hc"));
          });
    } else {
      encoder_callbacks_->addEncodedTrailers([](Http::HeaderMap& trailers) { trailers.insertGrpcStatus().value(std::string("2")); });
      // internal error
    }
    
  }

  return Http::FilterHeadersStatus::Continue;
}

static bool written = false;
Http::FilterDataStatus Http1BridgeFilter::encodeData(Buffer::Instance& data, bool) {
  std::cout << "encode data!" << std::endl;
  if (!written) {
  {
    Buffer::OwnedImpl buffer("\0");
    encoder_callbacks_->addEncodedData(buffer, false);
  }

  // length of the data, in big endian order
  uint32_t length = htonl(data.length());

  Buffer::OwnedImpl buffer;
  buffer.add("\0", 1);
  buffer.add(&length, 4);
  buffer.move(data);
  data.move(buffer);

  written = true;
  } 
  
  return Http::FilterDataStatus::Continue;
}

Http::FilterTrailersStatus Http1BridgeFilter::encodeTrailers(Http::HeaderMap& trailers) {
  if (do_stat_tracking_) {
    chargeStat(trailers);
  }


  // NOTE: We will still write the trailers, but the HTTP/1.1 codec will just eat them and end
  //       the chunk encoded response which is what we want.
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
