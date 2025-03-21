// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/view_transition/view_transition_utils.h"

#include "third_party/blink/renderer/core/dom/node_computed_style.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/paint/paint_layer.h"
#include "third_party/blink/renderer/core/view_transition/view_transition.h"
#include "third_party/blink/renderer/core/view_transition/view_transition_supplement.h"

namespace blink {

// static
ViewTransition* ViewTransitionUtils::GetActiveTransition(
    const Document& document) {
  auto* supplement = ViewTransitionSupplement::FromIfExists(document);
  if (!supplement) {
    return nullptr;
  }
  auto* transition = supplement->GetActiveTransition();
  if (!transition || transition->IsDone()) {
    return nullptr;
  }
  return transition;
}

// static
PseudoElement* ViewTransitionUtils::GetRootPseudo(const Document& document) {
  if (!document.documentElement()) {
    return nullptr;
  }

  PseudoElement* view_transition_pseudo =
      document.documentElement()->GetPseudoElement(kPseudoIdViewTransition);
  DCHECK(!view_transition_pseudo || GetActiveTransition(document));
  return view_transition_pseudo;
}

// static
VectorOf<std::unique_ptr<ViewTransitionRequest>>
ViewTransitionUtils::GetPendingRequests(const Document& document) {
  auto* supplement = ViewTransitionSupplement::FromIfExists(document);
  if (supplement) {
    return supplement->TakePendingRequests();
  }
  return {};
}

// static
bool ViewTransitionUtils::IsViewTransitionRoot(const LayoutObject& object) {
  return object.GetNode() &&
         object.GetNode()->GetPseudoId() == kPseudoIdViewTransition;
}

// static
bool ViewTransitionUtils::IsViewTransitionParticipant(
    const LayoutObject& object) {
  // Special case LayoutView to check the supplement directly.
  if (IsA<LayoutView>(object)) {
    return IsViewTransitionParticipantFromSupplement(object);
  }

  if (const Element* element = DynamicTo<Element>(object.GetNode())) {
    if (const ComputedStyle* style = element->GetComputedStyle()) {
      DCHECK_EQ(style->ElementIsViewTransitionParticipant(),
                IsViewTransitionParticipantFromSupplement(*element))
          << object.DebugName();
      return style->ElementIsViewTransitionParticipant();
    }
  }

  DCHECK(!IsViewTransitionParticipantFromSupplement(object));
  return false;
}

// static
bool ViewTransitionUtils::IsViewTransitionParticipantFromSupplement(
    const Element& element) {
  ViewTransition* transition = GetActiveTransition(element.GetDocument());
  return transition && transition->IsRepresentedViaPseudoElements(element);
}

// static
bool ViewTransitionUtils::IsViewTransitionParticipantFromSupplement(
    const LayoutObject& object) {
  ViewTransition* transition = GetActiveTransition(object.GetDocument());
  return transition && transition->IsRepresentedViaPseudoElements(object);
}

}  // namespace blink
