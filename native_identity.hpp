/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include "../../utils/directx.hpp"

namespace renodx::addons::dlss5::native_identity {

// Borrowed identity, valid only while the caller keeps the source object alive.
// QueryInterface(IUnknown) canonicalizes interface pointers; release its added
// reference before returning. Keep Streamline proxies intact, as the existing
// Present resource/device handling explicitly unwraps those where appropriate.
inline IUnknown* Get(IUnknown* object) {
  if (object == nullptr) return nullptr;
  renodx::utils::directx::NativeFromReShadeProxy(&object);
  IUnknown* identity = nullptr;
  if (FAILED(object->QueryInterface(IID_PPV_ARGS(&identity)))) return nullptr;
  identity->Release();
  return identity;
}

inline bool Same(IUnknown* left, IUnknown* right) {
  IUnknown* identity = Get(left);
  return identity != nullptr && identity == Get(right);
}

}  // namespace renodx::addons::dlss5::native_identity
