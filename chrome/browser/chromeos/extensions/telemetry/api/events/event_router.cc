// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/extensions/telemetry/api/events/event_router.h"

#include <tuple>
#include <utility>

#include "chrome/browser/chromeos/extensions/telemetry/api/events/event_observation_crosapi.h"
#include "chromeos/crosapi/mojom/telemetry_event_service.mojom.h"
#include "content/public/browser/browser_context.h"
#include "extensions/common/extension_id.h"
#include "mojo/public/cpp/bindings/pending_remote.h"

namespace chromeos {

EventRouter::EventRouter(content::BrowserContext* context)
    : browser_context_(context) {}

EventRouter::~EventRouter() = default;

mojo::PendingRemote<crosapi::mojom::TelemetryEventObserver>
EventRouter::GetPendingRemoteForCategoryAndExtension(
    crosapi::mojom::TelemetryEventCategoryEnum category,
    extensions::ExtensionId extension_id) {
  auto iter_extension = observers_.find(extension_id);
  if (iter_extension == observers_.end()) {
    iter_extension = observers_.emplace_hint(
        iter_extension, std::piecewise_construct,
        std::forward_as_tuple(extension_id), std::forward_as_tuple());
  }

  auto iter_category = iter_extension->second.find(category);
  if (iter_category == iter_extension->second.end()) {
    iter_category = iter_extension->second.emplace_hint(
        iter_category, std::piecewise_construct,
        std::forward_as_tuple(category),
        std::forward_as_tuple(std::make_unique<EventObservationCrosapi>(
            extension_id, browser_context_)));
  }

  return iter_category->second->GetRemote();
}

void EventRouter::ResetReceiversForExtension(
    extensions::ExtensionId extension_id) {
  observers_.erase(extension_id);
}

void EventRouter::ResetReceiversOfExtensionByCategory(
    extensions::ExtensionId extension_id,
    crosapi::mojom::TelemetryEventCategoryEnum category) {
  auto it = observers_.find(extension_id);
  if (it == observers_.end()) {
    return;
  }

  it->second.erase(category);
}

bool EventRouter::IsExtensionObservingForCategory(
    extensions::ExtensionId extension_id,
    crosapi::mojom::TelemetryEventCategoryEnum category) {
  auto it = observers_.find(extension_id);
  if (it == observers_.end()) {
    return false;
  }

  return it->second.contains(category);
}
}  // namespace chromeos
