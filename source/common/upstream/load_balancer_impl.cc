#include "common/upstream/load_balancer_impl.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "envoy/runtime/runtime.h"
#include "envoy/upstream/upstream.h"

#include "common/common/assert.h"
#include "common/common/stack_array.h"
#include "common/protobuf/utility.h"

namespace Envoy {
namespace Upstream {

namespace {
static const std::string RuntimeZoneEnabled = "upstream.zone_routing.enabled";
static const std::string RuntimeMinClusterSize = "upstream.zone_routing.min_cluster_size";
static const std::string RuntimePanicThreshold = "upstream.healthy_panic_threshold";

// Distributes load between priorities based on the per priority availability and the normalized
// total availability. Load is assigned to each priority according to how available each priority is
// adjusted for the normalized total availability.
//
// @param per_priority_load vector of loads that should be populated.
// @param per_priority_availability the percentage availability of each priority, used to determine
// how much load each priority can handle.
// @param total_load the amount of load that may be distributed. Will be updated with the amount of
// load reminaining after distribution.
// @param normalized_total_availability the total availability, up to a max of 100. Used to
// scale the load when the total availability is less than 100%.
// @return the first available priority and the remaining load
std::pair<int32_t, size_t> distributeLoad(PriorityLoad& per_priority_load,
                                          const std::vector<uint32_t>& per_priority_availability,
                                          size_t total_load, size_t normalized_total_availability) {
  int32_t first_available_priority = -1;
  for (size_t i = 0; i < per_priority_availability.size(); ++i) {
    if (first_available_priority < 0 && per_priority_availability[i] > 0) {
      first_available_priority = i;
    }
    // Now assign as much load as possible to the high priority levels and cease assigning load
    // when total_load runs out.
    per_priority_load[i] = std::min<uint32_t>(total_load, per_priority_availability[i] * 100 /
                                                              normalized_total_availability);
    total_load -= per_priority_load[i];
  }

  return {first_available_priority, total_load};
}

} // namespace

std::pair<uint32_t, bool>
LoadBalancerBase::choosePriority(uint64_t hash, const std::vector<uint32_t>& per_priority_load,
                                 const std::vector<uint32_t>& degraded_per_priority_load) {
  hash = hash % 100 + 1; // 1-100
  uint32_t aggregate_percentage_load = 0;
  // As with tryChooseLocalLocalityHosts, this can be refactored for efficiency
  // but O(N) is good enough for now given the expected number of priorities is
  // small.

  // We first attempt to select a priority based on healthy hosts.
  for (size_t priority = 0; priority < per_priority_load.size(); ++priority) {
    aggregate_percentage_load += per_priority_load[priority];
    if (hash <= aggregate_percentage_load) {
      return {priority, false};
    }
  }

  // If no priorities were selected due to health, attempt to select a degraded one.
  for (size_t priority = 0; priority < degraded_per_priority_load.size(); ++priority) {
    aggregate_percentage_load += degraded_per_priority_load[priority];
    if (hash <= aggregate_percentage_load) {
      return {priority, true};
    }
  }

  // The percentages should always add up to 100 but we have to have a return for the compiler.
  NOT_REACHED_GCOVR_EXCL_LINE;
}

LoadBalancerBase::LoadBalancerBase(const PrioritySet& priority_set, ClusterStats& stats,
                                   Runtime::Loader& runtime, Runtime::RandomGenerator& random,
                                   const envoy::api::v2::Cluster::CommonLbConfig& common_config)
    : stats_(stats), runtime_(runtime), random_(random),
      default_healthy_panic_percent_(PROTOBUF_PERCENT_TO_ROUNDED_INTEGER_OR_DEFAULT(
          common_config, healthy_panic_threshold, 100, 50)),
      priority_set_(priority_set) {
  for (auto& host_set : priority_set_.hostSetsPerPriority()) {
    recalculatePerPriorityState(host_set->priority(), priority_set_, per_priority_load_,
                                degraded_per_priority_load_, per_priority_health_,
                                per_priority_degraded_);
  }
  // Reclaculate panic mode for all levels.
  recalculatePerPriorityPanic();

  priority_set_.addMemberUpdateCb(
      [this](uint32_t priority, const HostVector&, const HostVector&) -> void {
        recalculatePerPriorityState(priority, priority_set_, per_priority_load_,
                                    degraded_per_priority_load_, per_priority_health_,
                                    per_priority_degraded_);
      });
  priority_set_.addMemberUpdateCb(
      [this](uint32_t priority, const HostVector&, const HostVector&) -> void {
        UNREFERENCED_PARAMETER(priority);
        recalculatePerPriorityPanic();
      });
}

// The following cases are handled by
// recalculatePerPriorityState and recalculatePerPriorityPanic methods (normalized total health is
// sum of all priorities' health values and capped at 100).
// - normalized total health is = 100%. It means there are enough healthy hosts to handle the load.
//   Do not enter panic mode, even if a specific priority has low number of healthy hosts.
// - normalized total health is < 100%. There are not enough healthy hosts to handle the load.
// Continue
//   distibuting the load among priority sets, but turn on panic mode for a given priority
//   if # of healthy hosts in priority set is low.
// - normalized total health is 0%. All hosts are down. Redirect 100% of traffic to P=0 and enable
// panic mode.

void LoadBalancerBase::recalculatePerPriorityState(uint32_t priority,
                                                   const PrioritySet& priority_set,
                                                   PriorityLoad& per_priority_load,
                                                   PriorityLoad& degraded_per_priority_load,
                                                   std::vector<uint32_t>& per_priority_health,
                                                   std::vector<uint32_t>& per_priority_degraded) {
  per_priority_load.resize(priority_set.hostSetsPerPriority().size());
  degraded_per_priority_load.resize(priority_set.hostSetsPerPriority().size());
  per_priority_health.resize(priority_set.hostSetsPerPriority().size());
  per_priority_degraded.resize(priority_set.hostSetsPerPriority().size());

  // Determine the health of the newly modified priority level.
  // Health ranges from 0-100, and is the ratio of healthy/degraded hosts to total hosts, modified
  // by the overprovisioning factor.
  HostSet& host_set = *priority_set.hostSetsPerPriority()[priority];
  per_priority_health[priority] = 0;
  if (host_set.hosts().size() > 0) {
    // Each priority level's health is ratio of healthy hosts to total number of hosts in a priority
    // multiplied by overprovisioning factor of 1.4 and capped at 100%. It means that if all
    // hosts are healthy that priority's health is 100%*1.4=140% and is capped at 100% which results
    // in 100%. If 80% of hosts are healty, that priority's health is still 100% (80%*1.4=112% and
    // capped at 100%).
    per_priority_health[priority] =
        std::min<uint32_t>(100, (host_set.overprovisioningFactor() *
                                 host_set.healthyHosts().size() / host_set.hosts().size()));

    // We perform the same computation for degraded hosts.
    per_priority_degraded[priority] =
        std::min<uint32_t>(100, (host_set.overprovisioningFactor() *
                                 host_set.degradedHosts().size() / host_set.hosts().size()));
  }

  // Now that we've updated health for the changed priority level, we need to calculate percentage
  // load for all priority levels.

  // First, determine if the load needs to be scaled relative to availability (healthy + degraded).
  // For example if there are 3 host sets with 10% / 20% / 10% health and 20% / 10% / 0% degraded
  // they will get 16% / 28% / 14% load to healthy hosts and 28% / 14% / 0% load to degraded hosts
  // to ensure total load adds up to 100. Note the first healthy priority is receiving 2% additional
  // load due to rounding.
  //
  // Sum of priority levels' health and degraded values may exceed 100, so it is capped at 100 and
  // referred as normalized total availability.
  const uint32_t normalized_total_availability =
      calculateNormalizedTotalAvailability(per_priority_health, per_priority_degraded);
  if (normalized_total_availability == 0) {
    // Everything is terrible. Send all load to P=0.
    // In this one case sumEntries(per_priority_load) != 100 since we sinkhole all traffic in P=0.
    per_priority_load[0] = 100;
    return;
  }

  // We start of with a total load of 100 and distribute it between priorities based on
  // availability. We first attempt to distribute this load to healthy priorities based on healthy
  // availability.
  const auto first_healthy_and_remaining =
      distributeLoad(per_priority_load, per_priority_health, 100, normalized_total_availability);

  // Using the remaining load after allocating load to healthy priorities, distribute it based on
  // degraded availability.
  const auto remaining_load_for_degraded = first_healthy_and_remaining.second;
  const auto first_degraded_and_remaining =
      distributeLoad(degraded_per_priority_load, per_priority_degraded, remaining_load_for_degraded,
                     normalized_total_availability);

  // Anything that remains should just be rounding errors, so allocate that to the first available
  // priority, either as healthy or degraded.
  const auto remaining_load = first_degraded_and_remaining.second;
  if (remaining_load != 0) {
    const auto first_healthy = first_healthy_and_remaining.first;
    const auto first_degraded = first_degraded_and_remaining.first;
    ASSERT(first_healthy != -1 || first_degraded != -1);

    // Attempt to allocate the remainder to the first healthy priority first. If no such priority
    // exist, allocate to the first degraded priority.
    ASSERT(remaining_load < per_priority_load.size() + per_priority_degraded.size());
    if (first_healthy != -1) {
      per_priority_load[first_healthy] += remaining_load;
    } else {
      degraded_per_priority_load[first_degraded] += remaining_load;
    }
  }

  // The allocated load between healthy and degraded should be exactly 100.
  ASSERT(100 == std::accumulate(per_priority_load.begin(), per_priority_load.end(), 0) +
                    std::accumulate(degraded_per_priority_load.begin(),
                                    degraded_per_priority_load.end(), 0));
}

// Method iterates through priority levels and turns on/off panic mode.
void LoadBalancerBase::recalculatePerPriorityPanic() {
  per_priority_panic_.resize(priority_set_.hostSetsPerPriority().size());

  // TODO(snowp): Right now panic thresholds doesn't work well with degraded.
  // The normalized_total_health takes degraded hosts into account, while
  // the subsequent panic calculation only looks at healthy hosts. Ideally
  // they should both take degraded into account.
  const uint32_t normalized_total_availability =
      calculateNormalizedTotalAvailability(per_priority_health_, per_priority_degraded_);

  if (normalized_total_availability == 0) {
    // Everything is terrible. All load should be to P=0. Turn on panic mode.
    ASSERT(per_priority_load_[0] == 100);
    per_priority_panic_[0] = true;
    return;
  }

  for (size_t i = 0; i < per_priority_health_.size(); ++i) {
    // For each level check if it should run in panic mode. Never set panic mode if
    // normalized total health is 100%, even when individual priority level has very low # of
    // healthy hosts.
    const HostSet& priority_host_set = *priority_set_.hostSetsPerPriority()[i];
    per_priority_panic_[i] =
        (normalized_total_availability == 100 ? false : isGlobalPanic(priority_host_set));
  }
}

std::pair<HostSet&, bool> LoadBalancerBase::chooseHostSet(LoadBalancerContext* context) {
  if (context) {
    const auto& per_priority_load =
        context->determinePriorityLoad(priority_set_, per_priority_load_);

    // TODO(snowp): pass degraded priority load to plugin.
    const auto priorityAndSource =
        choosePriority(random_.random(), per_priority_load, degraded_per_priority_load_);
    return {*priority_set_.hostSetsPerPriority()[priorityAndSource.first],
            priorityAndSource.second};
  }

  const auto priorityAndSource =
      choosePriority(random_.random(), per_priority_load_, degraded_per_priority_load_);
  return {*priority_set_.hostSetsPerPriority()[priorityAndSource.first], priorityAndSource.second};
}

ZoneAwareLoadBalancerBase::ZoneAwareLoadBalancerBase(
    const PrioritySet& priority_set, const PrioritySet* local_priority_set, ClusterStats& stats,
    Runtime::Loader& runtime, Runtime::RandomGenerator& random,
    const envoy::api::v2::Cluster::CommonLbConfig& common_config)
    : LoadBalancerBase(priority_set, stats, runtime, random, common_config),
      local_priority_set_(local_priority_set),
      routing_enabled_(PROTOBUF_PERCENT_TO_ROUNDED_INTEGER_OR_DEFAULT(
          common_config.zone_aware_lb_config(), routing_enabled, 100, 100)),
      min_cluster_size_(PROTOBUF_GET_WRAPPED_OR_DEFAULT(common_config.zone_aware_lb_config(),
                                                        min_cluster_size, 6U)) {
  ASSERT(!priority_set.hostSetsPerPriority().empty());
  resizePerPriorityState();
  priority_set_.addMemberUpdateCb(
      [this](uint32_t priority, const HostVector&, const HostVector&) -> void {
        // Make sure per_priority_state_ is as large as priority_set_.hostSetsPerPriority()
        resizePerPriorityState();
        // If P=0 changes, regenerate locality routing structures. Locality based routing is
        // disabled at all other levels.
        if (local_priority_set_ && priority == 0) {
          regenerateLocalityRoutingStructures();
        }
      });
  if (local_priority_set_) {
    // Multiple priorities are unsupported for local priority sets.
    // In order to support priorities correctly, one would have to make some assumptions about
    // routing (all local Envoys fail over at the same time) and use all priorities when computing
    // the locality routing structure.
    ASSERT(local_priority_set_->hostSetsPerPriority().size() == 1);
    local_priority_set_member_update_cb_handle_ = local_priority_set_->addMemberUpdateCb(
        [this](uint32_t priority, const HostVector&, const HostVector&) -> void {
          ASSERT(priority == 0);
          // If the set of local Envoys changes, regenerate routing for P=0 as it does priority
          // based routing.
          regenerateLocalityRoutingStructures();
        });
  }
}

ZoneAwareLoadBalancerBase::~ZoneAwareLoadBalancerBase() {
  if (local_priority_set_member_update_cb_handle_ != nullptr) {
    local_priority_set_member_update_cb_handle_->remove();
  }
}

void ZoneAwareLoadBalancerBase::regenerateLocalityRoutingStructures() {
  ASSERT(local_priority_set_);
  stats_.lb_recalculate_zone_structures_.inc();
  // resizePerPriorityState should ensure these stay in sync.
  ASSERT(per_priority_state_.size() == priority_set_.hostSetsPerPriority().size());

  // We only do locality routing for P=0
  uint32_t priority = 0;
  PerPriorityState& state = *per_priority_state_[priority];
  // Do not perform any calculations if we cannot perform locality routing based on non runtime
  // params.
  if (earlyExitNonLocalityRouting()) {
    state.locality_routing_state_ = LocalityRoutingState::NoLocalityRouting;
    return;
  }
  HostSet& host_set = *priority_set_.hostSetsPerPriority()[priority];
  ASSERT(host_set.healthyHostsPerLocality().hasLocalLocality());
  const size_t num_localities = host_set.healthyHostsPerLocality().get().size();
  ASSERT(num_localities > 0);

  // It is worth noting that all of the percentages calculated are orthogonal from
  // how much load this priority level receives, percentageLoad(priority).
  //
  // If the host sets are such that 20% of load is handled locally and 80% is residual, and then
  // half the hosts in all host sets go unhealthy, this priority set will
  // still send half of the incoming load to the local locality and 80% to residual.
  //
  // Basically, fariness across localities within a priority is guaranteed. Fairness across
  // localities across priorities is not.
  STACK_ARRAY(local_percentage, uint64_t, num_localities);
  calculateLocalityPercentage(localHostSet().healthyHostsPerLocality(), local_percentage.begin());
  STACK_ARRAY(upstream_percentage, uint64_t, num_localities);
  calculateLocalityPercentage(host_set.healthyHostsPerLocality(), upstream_percentage.begin());

  // If we have lower percent of hosts in the local cluster in the same locality,
  // we can push all of the requests directly to upstream cluster in the same locality.
  if (upstream_percentage[0] >= local_percentage[0]) {
    state.locality_routing_state_ = LocalityRoutingState::LocalityDirect;
    return;
  }

  state.locality_routing_state_ = LocalityRoutingState::LocalityResidual;

  // If we cannot route all requests to the same locality, calculate what percentage can be routed.
  // For example, if local percentage is 20% and upstream is 10%
  // we can route only 50% of requests directly.
  state.local_percent_to_route_ = upstream_percentage[0] * 10000 / local_percentage[0];

  // Local locality does not have additional capacity (we have already routed what we could).
  // Now we need to figure out how much traffic we can route cross locality and to which exact
  // locality we should route. Percentage of requests routed cross locality to a specific locality
  // needed be proportional to the residual capacity upstream locality has.
  //
  // residual_capacity contains capacity left in a given locality, we keep accumulating residual
  // capacity to make search for sampled value easier.
  // For example, if we have the following upstream and local percentage:
  // local_percentage: 40000 40000 20000
  // upstream_percentage: 25000 50000 25000
  // Residual capacity would look like: 0 10000 5000. Now we need to sample proportionally to
  // bucket sizes (residual capacity). For simplicity of finding where specific
  // sampled value is, we accumulate values in residual capacity. This is what it will look like:
  // residual_capacity: 0 10000 15000
  // Now to find a locality to route (bucket) we could simply iterate over residual_capacity
  // searching where sampled value is placed.
  state.residual_capacity_.resize(num_localities);

  // Local locality (index 0) does not have residual capacity as we have routed all we could.
  state.residual_capacity_[0] = 0;
  for (size_t i = 1; i < num_localities; ++i) {
    // Only route to the localities that have additional capacity.
    if (upstream_percentage[i] > local_percentage[i]) {
      state.residual_capacity_[i] =
          state.residual_capacity_[i - 1] + upstream_percentage[i] - local_percentage[i];
    } else {
      // Locality with index "i" does not have residual capacity, but we keep accumulating previous
      // values to make search easier on the next step.
      state.residual_capacity_[i] = state.residual_capacity_[i - 1];
    }
  }
}

void ZoneAwareLoadBalancerBase::resizePerPriorityState() {
  const uint32_t size = priority_set_.hostSetsPerPriority().size();
  while (per_priority_state_.size() < size) {
    // Note for P!=0, PerPriorityState is created with NoLocalityRouting and never changed.
    per_priority_state_.push_back(std::make_unique<PerPriorityState>());
  }
}

bool ZoneAwareLoadBalancerBase::earlyExitNonLocalityRouting() {
  // We only do locality routing for P=0.
  HostSet& host_set = *priority_set_.hostSetsPerPriority()[0];
  if (host_set.healthyHostsPerLocality().get().size() < 2) {
    return true;
  }

  // lb_local_cluster_not_ok is bumped for "Local host set is not set or it is
  // panic mode for local cluster".
  if (!host_set.healthyHostsPerLocality().hasLocalLocality() ||
      host_set.healthyHostsPerLocality().get()[0].empty()) {
    stats_.lb_local_cluster_not_ok_.inc();
    return true;
  }

  // Same number of localities should be for local and upstream cluster.
  if (host_set.healthyHostsPerLocality().get().size() !=
      localHostSet().healthyHostsPerLocality().get().size()) {
    stats_.lb_zone_number_differs_.inc();
    return true;
  }

  // Do not perform locality routing for small clusters.
  const uint64_t min_cluster_size =
      runtime_.snapshot().getInteger(RuntimeMinClusterSize, min_cluster_size_);
  if (host_set.healthyHosts().size() < min_cluster_size) {
    stats_.lb_zone_cluster_too_small_.inc();
    return true;
  }

  return false;
}

HostConstSharedPtr LoadBalancerBase::chooseHost(LoadBalancerContext* context) {
  HostConstSharedPtr host;
  const size_t max_attempts = context ? context->hostSelectionRetryCount() + 1 : 1;
  for (size_t i = 0; i < max_attempts; ++i) {
    host = chooseHostOnce(context);

    // If host selection failed or the host is accepted by the filter, return.
    // Otherwise, try again.
    // Note: in the future we might want to allow retrying when chooseHostOnce returns nullptr.
    if (!host || !context || !context->shouldSelectAnotherHost(*host)) {
      return host;
    }
  }

  // If we didnt find anything, return the last host.
  return host;
}

bool LoadBalancerBase::isGlobalPanic(const HostSet& host_set) {
  // TODO(snowp): This should also account for degraded hosts.
  uint64_t global_panic_threshold = std::min<uint64_t>(
      100, runtime_.snapshot().getInteger(RuntimePanicThreshold, default_healthy_panic_percent_));
  double healthy_percent = host_set.hosts().size() == 0
                               ? 0
                               : 100.0 * host_set.healthyHosts().size() / host_set.hosts().size();

  // If the % of healthy hosts in the cluster is less than our panic threshold, we use all hosts.
  if (healthy_percent < global_panic_threshold) {
    return true;
  }

  return false;
}

void ZoneAwareLoadBalancerBase::calculateLocalityPercentage(
    const HostsPerLocality& hosts_per_locality, uint64_t* ret) {
  uint64_t total_hosts = 0;
  for (const auto& locality_hosts : hosts_per_locality.get()) {
    total_hosts += locality_hosts.size();
  }

  size_t i = 0;
  for (const auto& locality_hosts : hosts_per_locality.get()) {
    ret[i++] = total_hosts > 0 ? 10000ULL * locality_hosts.size() / total_hosts : 0;
  }
}

uint32_t ZoneAwareLoadBalancerBase::tryChooseLocalLocalityHosts(const HostSet& host_set) {
  PerPriorityState& state = *per_priority_state_[host_set.priority()];
  ASSERT(state.locality_routing_state_ != LocalityRoutingState::NoLocalityRouting);

  // At this point it's guaranteed to be at least 2 localities & local exists.
  const size_t number_of_localities = host_set.healthyHostsPerLocality().get().size();
  ASSERT(number_of_localities >= 2U);
  ASSERT(host_set.healthyHostsPerLocality().hasLocalLocality());

  // Try to push all of the requests to the same locality first.
  if (state.locality_routing_state_ == LocalityRoutingState::LocalityDirect) {
    stats_.lb_zone_routing_all_directly_.inc();
    return 0;
  }

  ASSERT(state.locality_routing_state_ == LocalityRoutingState::LocalityResidual);

  // If we cannot route all requests to the same locality, we already calculated how much we can
  // push to the local locality, check if we can push to local locality on current iteration.
  if (random_.random() % 10000 < state.local_percent_to_route_) {
    stats_.lb_zone_routing_sampled_.inc();
    return 0;
  }

  // At this point we must route cross locality as we cannot route to the local locality.
  stats_.lb_zone_routing_cross_zone_.inc();

  // This is *extremely* unlikely but possible due to rounding errors when calculating
  // locality percentages. In this case just select random locality.
  if (state.residual_capacity_[number_of_localities - 1] == 0) {
    stats_.lb_zone_no_capacity_left_.inc();
    return random_.random() % number_of_localities;
  }

  // Random sampling to select specific locality for cross locality traffic based on the additional
  // capacity in localities.
  uint64_t threshold = random_.random() % state.residual_capacity_[number_of_localities - 1];

  // This potentially can be optimized to be O(log(N)) where N is the number of localities.
  // Linear scan should be faster for smaller N, in most of the scenarios N will be small.
  // TODO(htuch): is there a bug here when threshold == 0? Seems like we pick
  // local locality in that situation. Probably should start iterating at 1.
  int i = 0;
  while (threshold > state.residual_capacity_[i]) {
    i++;
  }

  return i;
}

ZoneAwareLoadBalancerBase::HostsSource
ZoneAwareLoadBalancerBase::hostSourceToUse(LoadBalancerContext* context) {
  auto host_set_and_source = chooseHostSet(context);

  // The second argument tells us whether or not we should be using degraded hosts from the
  // selected host set.
  const bool degraded_hosts = host_set_and_source.second;
  auto& host_set = host_set_and_source.first;
  HostsSource hosts_source;
  hosts_source.priority_ = host_set.priority();

  // If the selected host set has insufficient healthy hosts, return all hosts.
  if (per_priority_panic_[hosts_source.priority_]) {
    stats_.lb_healthy_panic_.inc();
    hosts_source.source_type_ = HostsSource::SourceType::AllHosts;
    return hosts_source;
  }

  // If we're doing locality weighted balancing, pick locality.
  const absl::optional<uint32_t> locality = host_set.chooseLocality();
  if (locality.has_value()) {
    hosts_source.source_type_ = localitySourceType(degraded_hosts);
    hosts_source.locality_index_ = locality.value();
    return hosts_source;
  }

  // If we've latched that we can't do priority-based routing, return healthy or degraded hosts
  // for the selected host set.
  if (per_priority_state_[host_set.priority()]->locality_routing_state_ ==
      LocalityRoutingState::NoLocalityRouting) {
    hosts_source.source_type_ = sourceType(degraded_hosts);
    return hosts_source;
  }

  // Determine if the load balancer should do zone based routing for this pick.
  if (!runtime_.snapshot().featureEnabled(RuntimeZoneEnabled, routing_enabled_)) {
    hosts_source.source_type_ = sourceType(degraded_hosts);
    return hosts_source;
  }

  if (isGlobalPanic(localHostSet())) {
    stats_.lb_local_cluster_not_ok_.inc();
    // If the local Envoy instances are in global panic, do not do locality
    // based routing.
    hosts_source.source_type_ = sourceType(degraded_hosts);
    return hosts_source;
  }

  hosts_source.source_type_ = localitySourceType(degraded_hosts);
  hosts_source.locality_index_ = tryChooseLocalLocalityHosts(host_set);
  return hosts_source;
}

const HostVector& ZoneAwareLoadBalancerBase::hostSourceToHosts(HostsSource hosts_source) {
  const HostSet& host_set = *priority_set_.hostSetsPerPriority()[hosts_source.priority_];
  switch (hosts_source.source_type_) {
  case HostsSource::SourceType::AllHosts:
    return host_set.hosts();
  case HostsSource::SourceType::HealthyHosts:
    return host_set.healthyHosts();
  case HostsSource::SourceType::DegradedHosts:
    return host_set.degradedHosts();
  case HostsSource::SourceType::LocalityHealthyHosts:
    return host_set.healthyHostsPerLocality().get()[hosts_source.locality_index_];
  case HostsSource::SourceType::LocalityDegradedHosts:
    return host_set.degradedHostsPerLocality().get()[hosts_source.locality_index_];
  default:
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
}

EdfLoadBalancerBase::EdfLoadBalancerBase(
    const PrioritySet& priority_set, const PrioritySet* local_priority_set, ClusterStats& stats,
    Runtime::Loader& runtime, Runtime::RandomGenerator& random,
    const envoy::api::v2::Cluster::CommonLbConfig& common_config)
    : ZoneAwareLoadBalancerBase(priority_set, local_priority_set, stats, runtime, random,
                                common_config),
      seed_(random_.random()) {
  // We fully recompute the schedulers for a given host set here on membership change, which is
  // consistent with what other LB implementations do (e.g. thread aware).
  // The downside of a full recompute is that time complexity is O(n * log n),
  // so we will need to do better at delta tracking to scale (see
  // https://github.com/envoyproxy/envoy/issues/2874).
  priority_set.addMemberUpdateCb(
      [this](uint32_t priority, const HostVector&, const HostVector&) { refresh(priority); });
}

void EdfLoadBalancerBase::initialize() {
  for (uint32_t priority = 0; priority < priority_set_.hostSetsPerPriority().size(); ++priority) {
    refresh(priority);
  }
}

void EdfLoadBalancerBase::refresh(uint32_t priority) {
  const auto add_hosts_source = [this](HostsSource source, const HostVector& hosts) {
    // Nuke existing scheduler if it exists.
    auto& scheduler = scheduler_[source] = Scheduler{};
    refreshHostSource(source);

    // Populate scheduler with host list.
    // TODO(mattklein123): We must build the EDF schedule even if all of the hosts are currently
    // weighted 1. This is because currently we don't refresh host sets if only weights change.
    // We should probably change this to refresh at all times. See the comment in
    // BaseDynamicClusterImpl::updateDynamicHostList about this.
    for (const auto& host : hosts) {
      // We use a fixed weight here. While the weight may change without
      // notification, this will only be stale until this host is next picked,
      // at which point it is reinserted into the EdfScheduler with its new
      // weight in chooseHost().
      scheduler.edf_.add(hostWeight(*host), host);
    }

    // Cycle through hosts to achieve the intended offset behavior.
    // TODO(htuch): Consider how we can avoid biasing towards earlier hosts in the schedule across
    // refreshes for the weighted case.
    if (!hosts.empty()) {
      for (uint32_t i = 0; i < seed_ % hosts.size(); ++i) {
        auto host = scheduler.edf_.pick();
        scheduler.edf_.add(hostWeight(*host), host);
      }
    }
  };

  // Populate EdfSchedulers for each valid HostsSource value for the host set at this priority.
  const auto& host_set = priority_set_.hostSetsPerPriority()[priority];
  add_hosts_source(HostsSource(priority, HostsSource::SourceType::AllHosts), host_set->hosts());
  add_hosts_source(HostsSource(priority, HostsSource::SourceType::HealthyHosts),
                   host_set->healthyHosts());
  add_hosts_source(HostsSource(priority, HostsSource::SourceType::DegradedHosts),
                   host_set->degradedHosts());
  for (uint32_t locality_index = 0;
       locality_index < host_set->healthyHostsPerLocality().get().size(); ++locality_index) {
    add_hosts_source(
        HostsSource(priority, HostsSource::SourceType::LocalityHealthyHosts, locality_index),
        host_set->healthyHostsPerLocality().get()[locality_index]);
  }
  for (uint32_t locality_index = 0;
       locality_index < host_set->degradedHostsPerLocality().get().size(); ++locality_index) {
    add_hosts_source(
        HostsSource(priority, HostsSource::SourceType::LocalityDegradedHosts, locality_index),
        host_set->degradedHostsPerLocality().get()[locality_index]);
  }
}

HostConstSharedPtr EdfLoadBalancerBase::chooseHostOnce(LoadBalancerContext* context) {
  const HostsSource hosts_source = hostSourceToUse(context);
  auto scheduler_it = scheduler_.find(hosts_source);
  // We should always have a scheduler for any return value from
  // hostSourceToUse() via the construction in refresh();
  ASSERT(scheduler_it != scheduler_.end());
  auto& scheduler = scheduler_it->second;

  // As has been commented in both EdfLoadBalancerBase::refresh and
  // BaseDynamicClusterImpl::updateDynamicHostList, we must do a runtime pivot here to determine
  // whether to use EDF or do unweighted (fast) selection.
  // TODO(mattklein123): As commented elsewhere, this is wasteful, and we should just refresh the
  // host set if any weights change. Additionally, it has the property that if all weights are
  // the same but not 1 (like 42), we will use the EDF schedule not the unweighted pick. This is
  // not optimal. If this is fixed, remove the note in the arch overview docs for the LR LB.
  if (stats_.max_host_weight_.value() != 1) {
    auto host = scheduler.edf_.pick();
    if (host != nullptr) {
      scheduler.edf_.add(hostWeight(*host), host);
    }
    return host;
  } else {
    const HostVector& hosts_to_use = hostSourceToHosts(hosts_source);
    if (hosts_to_use.size() == 0) {
      return nullptr;
    }
    return unweightedHostPick(hosts_to_use, hosts_source);
  }
}

HostConstSharedPtr LeastRequestLoadBalancer::unweightedHostPick(const HostVector& hosts_to_use,
                                                                const HostsSource&) {
  HostSharedPtr candidate_host = nullptr;
  for (uint32_t choice_idx = 0; choice_idx < choice_count_; ++choice_idx) {
    const int rand_idx = random_.random() % hosts_to_use.size();
    HostSharedPtr sampled_host = hosts_to_use[rand_idx];

    if (candidate_host == nullptr) {
      // Make a first choice to start the comparisons.
      candidate_host = sampled_host;
      continue;
    }

    const auto candidate_active_rq = candidate_host->stats().rq_active_.value();
    const auto sampled_active_rq = sampled_host->stats().rq_active_.value();
    if (sampled_active_rq < candidate_active_rq) {
      candidate_host = sampled_host;
    }
  }

  return candidate_host;
}

HostConstSharedPtr RandomLoadBalancer::chooseHostOnce(LoadBalancerContext* context) {
  const HostVector& hosts_to_use = hostSourceToHosts(hostSourceToUse(context));
  if (hosts_to_use.empty()) {
    return nullptr;
  }

  return hosts_to_use[random_.random() % hosts_to_use.size()];
}

} // namespace Upstream
} // namespace Envoy
