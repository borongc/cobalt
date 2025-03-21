// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/views/interaction/interactive_views_test.h"

#include <functional>

#include "base/strings/strcat.h"
#include "base/test/bind.h"
#include "build/build_config.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "ui/base/interaction/element_tracker.h"
#include "ui/base/interaction/interaction_sequence.h"
#include "ui/base/interaction/interaction_test_util.h"
#include "ui/base/test/ui_controls.h"
#include "ui/gfx/native_widget_types.h"
#include "ui/views/interaction/interaction_test_util_views.h"
#include "ui/views/view_tracker.h"

#if BUILDFLAG(IS_MAC)
#include "ui/base/interaction/interaction_test_util_mac.h"
#endif

namespace views::test {

using ui::test::internal::SpecifyElement;

namespace {

auto CreateTestUtil() {
  auto test_util = std::make_unique<ui::test::InteractionTestUtil>();
  test_util->AddSimulator(
      std::make_unique<views::test::InteractionTestUtilSimulatorViews>());
#if BUILDFLAG(IS_MAC)
  test_util->AddSimulator(
      std::make_unique<ui::test::InteractionTestUtilSimulatorMac>());
#endif
  return test_util;
}

}  // namespace

using ui::test::internal::kInteractiveTestPivotElementId;

InteractiveViewsTestApi::InteractiveViewsTestApi()
    : InteractiveViewsTestApi(
          std::make_unique<internal::InteractiveViewsTestPrivate>(
              CreateTestUtil())) {}

InteractiveViewsTestApi::InteractiveViewsTestApi(
    std::unique_ptr<internal::InteractiveViewsTestPrivate> private_test_impl)
    : InteractiveTestApi(std::move(private_test_impl)) {}
InteractiveViewsTestApi::~InteractiveViewsTestApi() = default;

ui::InteractionSequence::StepBuilder InteractiveViewsTestApi::NameView(
    base::StringPiece name,
    AbsoluteViewSpecifier spec) {
  return NameViewRelative(kInteractiveTestPivotElementId, name,
                          GetFindViewCallback(std::move(spec)));
}

// static
ui::InteractionSequence::StepBuilder InteractiveViewsTestApi::NameChildView(
    ElementSpecifier parent,
    base::StringPiece name,
    ChildViewSpecifier spec) {
  return std::move(
      NameViewRelative(parent, name, GetFindViewCallback(std::move(spec)))
          .SetDescription(
              base::StringPrintf("NameChildView( \"%s\" )", name.data())));
}

// static
ui::InteractionSequence::StepBuilder
InteractiveViewsTestApi::NameDescendantView(ElementSpecifier parent,
                                            base::StringPiece name,
                                            ViewMatcher matcher) {
  return std::move(
      NameViewRelative(
          parent, name,
          base::BindOnce(
              [](ViewMatcher matcher, View* ancestor) -> View* {
                auto* const result =
                    FindMatchingView(ancestor, matcher, /* recursive =*/true);
                if (!result) {
                  LOG(ERROR)
                      << "NameDescendantView(): No descendant matches matcher.";
                }
                return result;
              },
              matcher))
          .SetDescription(
              base::StringPrintf("NameDescendantView( \"%s\" )", name.data())));
}

InteractiveViewsTestApi::StepBuilder InteractiveViewsTestApi::ScrollIntoView(
    ElementSpecifier view) {
  return std::move(WithView(view, [](View* v) {
                     v->ScrollViewToVisible();
                   }).SetDescription("ScrollIntoView()"));
}

InteractiveViewsTestApi::StepBuilder InteractiveViewsTestApi::MoveMouseTo(
    ElementSpecifier reference,
    RelativePositionSpecifier position) {
  StepBuilder step;
  step.SetDescription("MoveMouseTo()");
  SpecifyElement(step, reference);
  step.SetStartCallback(base::BindOnce(
      [](InteractiveViewsTestApi* test, RelativePositionCallback pos_callback,
         ui::InteractionSequence* seq, ui::TrackedElement* el) {
        test->test_impl().mouse_error_message_.clear();
        if (!test->mouse_util().PerformGestures(
                test->test_impl().GetWindowHintFor(el),
                InteractionTestUtilMouse::MoveTo(
                    std::move(pos_callback).Run(el)))) {
          seq->FailForTesting();
        }
      },
      base::Unretained(this), GetPositionCallback(std::move(position))));
  return step;
}

InteractiveViewsTestApi::StepBuilder InteractiveViewsTestApi::MoveMouseTo(
    AbsolutePositionSpecifier position) {
  return MoveMouseTo(kInteractiveTestPivotElementId,
                     GetPositionCallback(std::move(position)));
}

InteractiveViewsTestApi::StepBuilder InteractiveViewsTestApi::ClickMouse(
    ui_controls::MouseButton button,
    bool release) {
  StepBuilder step;
  step.SetDescription("ClickMouse()");
  step.SetElementID(kInteractiveTestPivotElementId);
  step.SetStartCallback(base::BindOnce(
      [](InteractiveViewsTestApi* test, ui_controls::MouseButton button,
         bool release, ui::InteractionSequence* seq, ui::TrackedElement* el) {
        test->test_impl().mouse_error_message_.clear();
        if (!test->mouse_util().PerformGestures(
                test->test_impl().GetWindowHintFor(el),
                release ? InteractionTestUtilMouse::Click(button)
                        : InteractionTestUtilMouse::MouseGestures{
                              InteractionTestUtilMouse::MouseDown(button)})) {
          seq->FailForTesting();
        }
      },
      base::Unretained(this), button, release));
  step.SetMustRemainVisible(false);
  return step;
}

InteractiveViewsTestApi::StepBuilder InteractiveViewsTestApi::DragMouseTo(
    ElementSpecifier reference,
    RelativePositionSpecifier position,
    bool release) {
  StepBuilder step;
  step.SetDescription("DragMouseTo()");
  SpecifyElement(step, reference);
  step.SetStartCallback(base::BindOnce(
      [](InteractiveViewsTestApi* test, RelativePositionCallback pos_callback,
         bool release, ui::InteractionSequence* seq, ui::TrackedElement* el) {
        test->test_impl().mouse_error_message_.clear();
        const gfx::Point target = std::move(pos_callback).Run(el);
        if (!test->mouse_util().PerformGestures(
                test->test_impl().GetWindowHintFor(el),
                release ? InteractionTestUtilMouse::DragAndRelease(target)
                        : InteractionTestUtilMouse::DragAndHold(target))) {
          seq->FailForTesting();
        }
      },
      base::Unretained(this), GetPositionCallback(std::move(position)),
      release));
  return step;
}

InteractiveViewsTestApi::StepBuilder InteractiveViewsTestApi::DragMouseTo(
    AbsolutePositionSpecifier position,
    bool release) {
  return DragMouseTo(kInteractiveTestPivotElementId,
                     GetPositionCallback(std::move(position)), release);
}

InteractiveViewsTestApi::StepBuilder InteractiveViewsTestApi::ReleaseMouse(
    ui_controls::MouseButton button) {
  StepBuilder step;
  step.SetDescription("ReleaseMouse()");
  step.SetElementID(kInteractiveTestPivotElementId);
  step.SetStartCallback(base::BindOnce(
      [](InteractiveViewsTestApi* test, ui_controls::MouseButton button,
         ui::InteractionSequence* seq, ui::TrackedElement* el) {
        test->test_impl().mouse_error_message_.clear();
        if (!test->mouse_util().PerformGestures(
                test->test_impl().GetWindowHintFor(el),
                InteractionTestUtilMouse::MouseUp(button))) {
          return seq->FailForTesting();
        }
      },
      base::Unretained(this), button));
  step.SetMustRemainVisible(false);
  return step;
}

// static
InteractiveViewsTestApi::FindViewCallback
InteractiveViewsTestApi::GetFindViewCallback(AbsoluteViewSpecifier spec) {
  if (View** view = absl::get_if<View*>(&spec)) {
    CHECK(*view) << "NameView(View*): view must be set.";
    return base::BindOnce(
        [](const std::unique_ptr<ViewTracker>& ref, View*) {
          LOG_IF(ERROR, !ref->view()) << "NameView(View*): view ceased to be "
                                         "valid before step was executed.";
          return ref->view();
        },
        std::make_unique<ViewTracker>(*view));
  }

  if (std::reference_wrapper<View*>* view =
          absl::get_if<std::reference_wrapper<View*>>(&spec)) {
    return base::BindOnce(
        [](std::reference_wrapper<View*> view, View*) {
          LOG_IF(ERROR, !view.get())
              << "NameView(ref(View*)): view pointer is null.";
          return view.get();
        },
        *view);
  }

  return base::RectifyCallback<FindViewCallback>(
      std::move(absl::get<base::OnceCallback<View*()>>(spec)));
}

// static
InteractiveViewsTestApi::FindViewCallback
InteractiveViewsTestApi::GetFindViewCallback(ChildViewSpecifier spec) {
  if (size_t* index = absl::get_if<size_t>(&spec)) {
    return base::BindOnce(
        [](size_t index, View* parent) -> View* {
          if (index >= parent->children().size()) {
            LOG(ERROR) << "NameChildView(int): Child index out of bounds; got "
                       << index << " but only " << parent->children().size()
                       << " children.";
            return nullptr;
          }
          return parent->children()[index];
        },
        *index);
  }

  return base::BindOnce(
      [](ViewMatcher matcher, View* parent) -> View* {
        auto* const result =
            FindMatchingView(parent, matcher, /*recursive =*/false);
        LOG_IF(ERROR, !result)
            << "NameChildView(ViewMatcher): No child matches matcher.";
        return result;
      },
      absl::get<ViewMatcher>(spec));
}

// static
View* InteractiveViewsTestApi::FindMatchingView(const View* from,
                                                ViewMatcher& matcher,
                                                bool recursive) {
  for (auto* const child : from->children()) {
    if (matcher.Run(child))
      return child;
    if (recursive) {
      auto* const result = FindMatchingView(child, matcher, true);
      if (result)
        return result;
    }
  }
  return nullptr;
}

void InteractiveViewsTestApi::SetContextWidget(Widget* widget) {
  context_widget_ = widget;
  if (widget) {
    CHECK(!test_impl().mouse_util_)
        << "Changing the context widget during a test is not supported.";
    test_impl().mouse_util_ =
        std::make_unique<InteractionTestUtilMouse>(widget);
  } else {
    test_impl().mouse_util_.reset();
  }
}

// static
InteractiveViewsTestApi::RelativePositionCallback
InteractiveViewsTestApi::GetPositionCallback(AbsolutePositionSpecifier spec) {
  if (auto* point = absl::get_if<gfx::Point>(&spec)) {
    return base::BindOnce([](gfx::Point p, ui::TrackedElement*) { return p; },
                          *point);
  }

  if (auto** point = absl::get_if<gfx::Point*>(&spec)) {
    return base::BindOnce([](gfx::Point* p, ui::TrackedElement*) { return *p; },
                          base::Unretained(*point));
  }

  CHECK(absl::holds_alternative<AbsolutePositionCallback>(spec));
  return base::RectifyCallback<RelativePositionCallback>(
      std::move(absl::get<AbsolutePositionCallback>(spec)));
}

// static
InteractiveViewsTestApi::RelativePositionCallback
InteractiveViewsTestApi::GetPositionCallback(RelativePositionSpecifier spec) {
  if (auto* cb = absl::get_if<RelativePositionCallback>(&spec)) {
    return std::move(*cb);
  }

  CHECK(absl::holds_alternative<CenterPoint>(spec));
  return base::BindOnce([](ui::TrackedElement* el) {
    CHECK(el->IsA<views::TrackedElementViews>());
    return el->AsA<views::TrackedElementViews>()
        ->view()
        ->GetBoundsInScreen()
        .CenterPoint();
  });
}

}  // namespace views::test
