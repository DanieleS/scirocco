#pragma once

// Source-level fixtures compile extracted Windows mutation functions, which now
// enter display_helper::borrow::native_mutation_scope_t first. Reuse the
// production local process gate; helper mode fails closed because these
// non-Windows harnesses do not provide its named mutex (the same stand-in as
// test_virtual_display_recovery_transport.py).

#include <atomic>
#include <functional>
#include <utility>

#include "src/platform/windows/display_helper_borrow_gate.h"

namespace display_helper::borrow {
  inline std::atomic_bool &helper_process() {
    static std::atomic_bool active {false};
    return active;
  }

  class native_mutation_scope_t {
  public:
    native_mutation_scope_t() = default;
    native_mutation_scope_t(const native_mutation_scope_t &) = delete;
    native_mutation_scope_t &operator=(const native_mutation_scope_t &) = delete;
    native_mutation_scope_t(native_mutation_scope_t &&) = default;
    explicit operator bool() const { return static_cast<bool>(local_); }
    static native_mutation_scope_t enter(bool wait, const std::function<bool()> &cancelled = {}) {
      auto local = wait ? process_gate().wait_enter(cancelled) : process_gate().try_enter();
      if (!local || helper_process().load(std::memory_order_acquire)) return {};
      return native_mutation_scope_t {std::move(local)};
    }

  private:
    explicit native_mutation_scope_t(gate_t::scope_t local): local_(std::move(local)) {}
    gate_t::scope_t local_;
  };
}  // namespace display_helper::borrow
