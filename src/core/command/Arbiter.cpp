#include "core/command/Arbiter.h"

#include <algorithm>

#include "core/common/Optional.h"

namespace gw::core::command {

namespace {

// a が b より先に引き継ぐ（優先度が高い、同じなら新しい）
bool before(const Claim& a, const Claim& b) {
  if (a.priority != b.priority) {
    return at_least(a.priority, b.priority);
  }
  return a.order > b.order;
}

}  // namespace

ClaimResult Arbiter::claim(const ActuatorKey& key, Claim c, SteadyTime now) {
  auto& slot = slots_[key];
  if (slot.owner && slot.owner->lease_until <= now) {
    slot.owner.reset();  // 期限切れの所有者は無いものとみなす
  }
  if (slot.owner && slot.owner->owner != c.owner && !at_least(c.priority, slot.owner->priority)) {
    return ClaimResult{false, slot.owner->priority};
  }
  c.order = next_order_++;
  std::erase_if(slot.waiting, [&](const Claim& w) { return w.owner == c.owner; });
  if (slot.owner && slot.owner->owner != c.owner && slot.owner->lease_until > now) {
    slot.waiting.push_back(std::move(*slot.owner));  // 奪われた所有者は期限まで待つ（#24）
  }
  slot.owner = std::move(c);
  return ClaimResult{true, std::nullopt};
}

std::optional<Claim> Arbiter::take_next(Slot& slot, SteadyTime now) {
  std::erase_if(slot.waiting, [&](const Claim& w) { return w.lease_until <= now; });
  if (slot.waiting.empty()) {
    return std::nullopt;
  }
  auto best = std::min_element(slot.waiting.begin(), slot.waiting.end(), before);
  Claim next = std::move(*best);
  slot.waiting.erase(best);
  return next;
}

std::optional<OwnerChange> Arbiter::remove_if(const ActuatorKey& key, SteadyTime now,
                                              const auto& predicate) {
  const auto it = slots_.find(key);
  if (it == slots_.end()) {
    return std::nullopt;
  }
  auto& slot = it->second;
  std::erase_if(slot.waiting, predicate);
  if (!slot.owner || !predicate(*slot.owner)) {
    return std::nullopt;
  }
  slot.owner = take_next(slot, now);
  OwnerChange change{key, slot.owner};
  if (!slot.owner && slot.waiting.empty()) {
    slots_.erase(it);
  }
  return change;
}

std::optional<OwnerChange> Arbiter::release(const ActuatorKey& key, const std::string& owner,
                                            SteadyTime now) {
  return remove_if(key, now, [&](const Claim& c) { return c.owner == owner; });
}

std::optional<OwnerChange> Arbiter::release_for(const ActuatorKey& key, Priority priority,
                                                SteadyTime now) {
  const auto it = slots_.find(key);
  if (it == slots_.end()) {
    return std::nullopt;
  }
  std::erase_if(it->second.waiting, [&](const Claim& c) { return c.priority == priority; });
  const auto* current = owner(key);
  if (current == nullptr || !at_least(priority, current->priority)) {
    return std::nullopt;
  }
  const auto name = current->owner;
  return release(key, name, now);
}

std::vector<OwnerChange> Arbiter::expire(SteadyTime now) {
  std::vector<OwnerChange> changes;
  std::vector<ActuatorKey> keys;
  keys.reserve(slots_.size());
  for (const auto& [key, slot] : slots_) {
    keys.push_back(key);
  }
  for (const auto& key : keys) {
    if (auto change = remove_if(key, now, [&](const Claim& c) { return c.lease_until <= now; })) {
      changes.push_back(std::move(*change));
    }
  }
  return changes;
}

void Arbiter::extend(const ActuatorKey& key, const std::string& owner, SteadyTime lease_until) {
  const auto it = slots_.find(key);
  if (it == slots_.end()) {
    return;
  }
  if (auto& current = it->second.owner; current.has_value() && current.value().owner == owner) {
    current.value().lease_until = lease_until;
  }
  for (auto& w : it->second.waiting) {
    if (w.owner == owner) {
      w.lease_until = lease_until;
    }
  }
}

const Claim* Arbiter::owner(const ActuatorKey& key) const {
  const auto it = slots_.find(key);
  return it == slots_.end() ? nullptr : opt_ptr(it->second.owner);
}

std::size_t Arbiter::waiting(const ActuatorKey& key) const {
  const auto it = slots_.find(key);
  return it == slots_.end() ? 0 : it->second.waiting.size();
}

std::optional<SteadyTime> Arbiter::next_expiry() const {
  std::optional<SteadyTime> next;
  for (const auto& [key, slot] : slots_) {
    if (const auto* o = opt_ptr(slot.owner)) {
      const auto* n = opt_ptr(next);
      if (n == nullptr || o->lease_until < *n) {
        next = o->lease_until;
      }
    }
    for (const auto& w : slot.waiting) {
      if (!next.has_value() || w.lease_until < next.value()) {
        next = w.lease_until;
      }
    }
  }
  return next;
}

}  // namespace gw::core::command
