// Copyright 2025 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_H5VCC_STORAGE_H_5_VCC_STORAGE_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_H5VCC_STORAGE_H_5_VCC_STORAGE_H_

#include "third_party/blink/renderer/core/execution_context/execution_context_lifecycle_observer.h"
#include "third_party/blink/renderer/modules/modules_export.h"
#include "third_party/blink/renderer/platform/bindings/script_wrappable.h"
#include "third_party/blink/renderer/platform/supplementable.h"

namespace blink {

class LocalDOMWindow;

class MODULES_EXPORT H5vccStorage final
    : public ScriptWrappable,
      public ExecutionContextLifecycleObserver,
      public Supplementable<H5vccStorage> {
  DEFINE_WRAPPERTYPEINFO();

 public:
  explicit H5vccStorage(LocalDOMWindow&);

  void ContextDestroyed() override;

  void Trace(Visitor*) const override;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_H5VCC_STORAGE_H_5_VCC_STORAGE_H_
