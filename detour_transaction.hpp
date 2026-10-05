/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <windows.h>
#include <detours.h>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace renodx::addons::dlss5::detour_transaction {

// Serializes this addon's transaction groups through commit/abort. A caller
// outside this mutex may already own a transaction in the linked Detours copy;
// BeginTransaction must then fail without aborting that caller's transaction.
inline std::mutex transaction_mutex;
inline std::atomic_uint64_t transaction_contended{0};

inline std::mutex& TransactionMutex() { return transaction_mutex; }

inline bool BeginTransaction() {
  const LONG result = DetourTransactionBegin();
  if (result == ERROR_INVALID_OPERATION) {
    transaction_contended.fetch_add(1, std::memory_order_relaxed);
  }
  return result == NO_ERROR;
}

}  // namespace renodx::addons::dlss5::detour_transaction
