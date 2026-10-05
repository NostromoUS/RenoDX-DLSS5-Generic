/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#include "../detour_transaction.hpp"
#include "../native_identity.hpp"

#include <cassert>
#include <cstdio>
#include <thread>

namespace {

struct __declspec(uuid("51C21F4A-24AD-40AF-A15C-C125113766CA")) Left : IUnknown {};
struct __declspec(uuid("E3137358-E54C-4DD0-A450-E389220F821B")) Right : IUnknown {};

// Stack-owned fixtures: Release records reference balance without deleting.
class Object final : public Left, public Right {
 public:
  ULONG references = 1;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
    if (output == nullptr) return E_POINTER;
    *output = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(Left)) {
      *output = static_cast<Left*>(this);
    } else if (iid == __uuidof(Right)) {
      *output = static_cast<Right*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override { return --references; }
};

class Proxy final : public IUnknown {
 public:
  explicit Proxy(IUnknown* base) : base_(base) { base_->AddRef(); }
  ~Proxy() { base_->Release(); }
  ULONG references = 1;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
    if (output == nullptr) return E_POINTER;
    *output = nullptr;
    if (iid == __uuidof(renodx::utils::directx::ReShadeRetrieveBaseInterface)) {
      return base_->QueryInterface(__uuidof(IUnknown), output);
    }
    if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
    *output = static_cast<IUnknown*>(this);
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
  ULONG STDMETHODCALLTYPE Release() override { return --references; }

 private:
  IUnknown* base_;
};

void TestIdentity() {
  namespace identity = renodx::addons::dlss5::native_identity;
  Object object;
  Object other;
  auto* left = static_cast<Left*>(&object);
  auto* right = static_cast<Right*>(&object);
  assert(static_cast<IUnknown*>(left) != static_cast<IUnknown*>(right));
  assert(identity::Same(left, right));
  assert(!identity::Same(left, static_cast<Left*>(&other)));
  assert(identity::Get(nullptr) == nullptr);
  assert(!identity::Same(nullptr, nullptr));
  assert(!identity::Same(left, nullptr));
  assert(object.references == 1 && other.references == 1);
  {
    Proxy proxy(right);
    assert(identity::Same(&proxy, left));
    assert(identity::Get(&proxy) == static_cast<IUnknown*>(left));
    assert(proxy.references == 1 && object.references == 2);
  }
  assert(object.references == 1);
}

void TestTransactionContention() {
  namespace transaction = renodx::addons::dlss5::detour_transaction;
  // Simulate another caller sharing this Detours copy without our mutex.
  assert(DetourTransactionBegin() == NO_ERROR);
  const auto before = transaction::transaction_contended.load();
  bool started = true;
  std::thread contender([&] {
    std::lock_guard lock(transaction::TransactionMutex());
    started = transaction::BeginTransaction();
    if (started) DetourTransactionAbort();
  });
  contender.join();
  assert(!started);
  assert(transaction::transaction_contended.load() == before + 1);
  // The failed attempt must not have aborted the original transaction.
  assert(DetourTransactionAbort() == NO_ERROR);
  std::lock_guard lock(transaction::TransactionMutex());
  assert(transaction::BeginTransaction());
  assert(DetourUpdateThread(GetCurrentThread()) == NO_ERROR);
  assert(DetourTransactionCommit() == NO_ERROR);
}

}  // namespace

int main() {
  TestIdentity();
  TestTransactionContention();
  std::puts("Windows COM identity and Detours compatibility tests passed");
}
