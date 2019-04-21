#include "common/upstream/host_utility.h"

#include <string>

namespace Envoy {
namespace Upstream {

std::string HostUtility::healthFlagsToString(const Endpoint& endpoint) {

  std::string ret;
  if (endpoint.healthFlagGet(Endpoint::EndpointHealth::FAILED_ACTIVE_HC)) {
    ret += "/failed_active_hc";
  }

  return ret;
}
std::string HostUtility::healthFlagsToString(const Host& host) {
  if (host.health() == Host::Health::Healthy) {
    return "healthy";
  }

  std::string ret = healthFlagsToString(*host.endpoint());

  if (host.healthFlagGet(Host::HealthFlag::FAILED_OUTLIER_CHECK)) {
    ret += "/failed_outlier_check";
  }

  if (host.healthFlagGet(Host::HealthFlag::FAILED_EDS_HEALTH)) {
    ret += "/failed_eds_health";
  }

  return ret;
}

} // namespace Upstream
} // namespace Envoy
