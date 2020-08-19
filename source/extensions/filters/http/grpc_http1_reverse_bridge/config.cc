#include "extensions/filters/http/grpc_http1_reverse_bridge/config.h"

#include "envoy/extensions/filters/http/grpc_http1_reverse_bridge/v3/config.pb.h"
#include "envoy/extensions/filters/http/grpc_http1_reverse_bridge/v3/config.pb.validate.h"
#include "envoy/registry/registry.h"

#include "extensions/filters/http/grpc_http1_reverse_bridge/filter.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace GrpcHttp1ReverseBridge {

Http::FilterFactoryCb Config::createFilterFactoryFromProtoTyped(
    const envoy::extensions::filters::http::grpc_http1_reverse_bridge::v3::FilterConfig& config,
    const std::string&, Server::Configuration::FactoryContext&) {

  // TODO(snowp): Remove as content_type is removed.
  if (!config.content_type().empty() &&
      (config.has_content_type_handling() || !config.upstream_response_validation().empty())) {
    throw EnvoyException(
        "Cannot set content_type_handling or upstream_response_validation if content_type is set.");
  }

  FilterConfig filter_config(config);
  return [filter_config](Envoy::Http::FilterChainFactoryCallbacks& callbacks) -> void {
    callbacks.addStreamFilter(std::make_shared<Filter>(filter_config));
  };
}

Router::RouteSpecificFilterConfigConstSharedPtr Config::createRouteSpecificFilterConfigTyped(
    const envoy::extensions::filters::http::grpc_http1_reverse_bridge::v3::FilterConfigPerRoute&
        proto_config,
    Server::Configuration::ServerFactoryContext&, ProtobufMessage::ValidationVisitor&) {
  return std::make_shared<FilterConfigPerRoute>(proto_config);
}

/**
 * Static registration for the grpc http1 reverse bridge filter. @see RegisterFactory.
 */
REGISTER_FACTORY(Config, Server::Configuration::NamedHttpFilterConfigFactory);

} // namespace GrpcHttp1ReverseBridge
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
