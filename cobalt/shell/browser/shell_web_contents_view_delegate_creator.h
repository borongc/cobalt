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

#ifndef COBALT_SHELL_BROWSER_SHELL_WEB_CONTENTS_VIEW_DELEGATE_CREATOR_H_
#define COBALT_SHELL_BROWSER_SHELL_WEB_CONTENTS_VIEW_DELEGATE_CREATOR_H_

#include <memory>

namespace content {

class WebContents;
class WebContentsViewDelegate;

std::unique_ptr<WebContentsViewDelegate> CreateShellWebContentsViewDelegate(
    WebContents* web_contents);

}  // namespace content

#endif  // COBALT_SHELL_BROWSER_SHELL_WEB_CONTENTS_VIEW_DELEGATE_CREATOR_H_
