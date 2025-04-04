// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_PROFILES_PROFILE_PICKER_INTERACTIVE_UITEST_BASE_H_
#define CHROME_BROWSER_UI_VIEWS_PROFILES_PROFILE_PICKER_INTERACTIVE_UITEST_BASE_H_

#include "chrome/browser/ui/views/profiles/profile_picker_test_base.h"

namespace ui {
enum KeyboardCode;
}

// Mixin adding helpers to write interactive ui tests focused on
// `ProfilePickerView`.
class WithProfilePickerInteractiveUiTestHelpers
    : public WithProfilePickerTestHelpers {
 public:
  void SendCloseWindowKeyboardCommand();

  void SendBackKeyboardCommand();

  void SendToggleFullscreenKeyboardCommand();

#if BUILDFLAG(IS_MAC)
  void SendQuitAppKeyboardCommand();
#endif

  void SendKeyPress(ui::KeyboardCode key,
                    bool control,
                    bool shift,
                    bool alt,
                    bool command);
};

#endif  // CHROME_BROWSER_UI_VIEWS_PROFILES_PROFILE_PICKER_INTERACTIVE_UITEST_BASE_H_
