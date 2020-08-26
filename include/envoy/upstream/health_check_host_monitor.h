#pragma once

#include <memory>

#include "envoy/common/pure.h"

namespace Envoy {
namespace Upstream {

/**
 * A monitor for "passive" health check events that might happen on every thread. For example, if a
 * special HTTP header is received, the data plane may decide to fast fail a host to avoid waiting
 * for the full Health Check interval to elapse before determining the host is active health check
 * failed.
 */
class HealthCheckHostMonitor {
public:
  virtual ~HealthCheckHostMonitor() = default;

  /**
   * Mark the host as unhealthy. Note that this may not be immediate as events may need to be
   * propagated between multiple threads.
   * @param immediate bool whether the endpoint should be removed immediately, regardless of whether it still exists in service discovery.
   */
  virtual void setUnhealthy(bool immediate) PURE;

  /**
   * Notify the monitor that this host should no longer be considered for immediate failure. This makes it possible to clear the flag
   * in cases where the service discovery data remains the same as an upstream is restarted/redeployed.
   */
  virtual void clearImmediateHealthCheckFailure() PURE;
};

using HealthCheckHostMonitorPtr = std::unique_ptr<HealthCheckHostMonitor>;

} // namespace Upstream
} // namespace Envoy
