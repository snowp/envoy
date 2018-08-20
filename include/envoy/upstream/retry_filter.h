#pragma once

#include "envoy/upstream/upstream.h"

namespace Envoy {
namespace Upstream {

// Redeclare this here in order to get around cyclical dependencies.
typedef std::vector<uint32_t> PriorityLoad;

/**
 * This class is used to optionally modify the Prioirtyload when selecting a priority for
 * a retry attempt.
 */
class RetryPriorityFilter {
public:
  virtual ~RetryPriorityFilter() {}

  /**
   * Determines what PriorityLoad to use for a retry attempt.
   *
   * @param priority_state current state of cluster.
   * @param original_priority the unmodified PriorityLoad.
   * @param attempted_hosts an ordered list of hosts that have been previously attempted.
   * @return a reference to the PriorityLoad to use. Return original_priority if no changes should be made.
   */
  virtual PriorityLoad& determinePriorityLoad(const PriorityState& priority_state,
                                              const PriorityLoad& original_priority,
                                              const HostVector& attempted_hosts) PURE;
};

typedef std::shared_ptr<RetryPriorityFilter> RetryPriorityFilterSharedPtr;

/**
 * Used to optionally reject a select host during retries. 
 */
class RetryHostFilter {
public:
  virtual ~RetryHostFilter() {}

  /**
   * Determines whether a host should be rejected during retries.
   *
   * @param candidate_host the host to either reject or accept.
   * @param attempted_hosts an ordered list of hosts that have been previously attempted.
   * @return whether the host should be rejected and host selection reattempted.
   */
  virtual bool shouldSelectAnotherHost(const Host& candidate_host,
                                       const HostVector& attempted_hosts) PURE;
};

typedef std::shared_ptr<RetryPriorityFilter> RetryHostFilterSharedPtr;

/**
 * Callbacks given to a RetryPriorityFilterFactory that allows adding retry filters.
 */
class RetryPriorityFilterFactoryCallbacks {
public:
  virtual ~RetryPriorityFilterFactoryCallbacks() {}

  /**
   * Called by the factory to add a RetryPriorityFilter.
   */
  virtual void addRetryFilter(RetryPriorityFilterSharedPtr filter) PURE;
};

/**
 * Callbacks given to a RetryHostFilterFactory that allows adding retry filters.
 */
class RetryHostFilterFactoryCallbacks {
public:
  virtual ~RetryHostFilterFactoryCallbacks() {}

  /**
   * Called by the factory to add a RetryHostFilter.
   */
  virtual void addRetryFilter(RetryHostFilterSharedPtr filter) PURE;
};

/**
 * Factory for RetryPriorityFilter.
 */
class RetryPriorityFilterFactory {
public:
  virtual ~RetryPriorityFilterFactory() {}

  virtual void createRetryFilter(RetryPriorityFilterFactoryCallbacks& callbacks) PURE;
};

/**
 * Factory for RetryHostFilter.
 */
class RetryHostFilterFactory {
public:
  virtual ~RetryHostFilterFactory() {}

  virtual void createRetryFilter(RetryHostFilterFactoryCallbacks& callbacks) PURE;
};

} // namespace Upstream
} // namespace Envoy
