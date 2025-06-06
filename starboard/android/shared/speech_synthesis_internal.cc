// Copyright 2020 The Cobalt Authors. All Rights Reserved.
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

#include <android/native_activity.h>

#include "starboard/android/shared/application_android.h"

// Must come after all headers that specialize FromJniType() / ToJniType().
#include "cobalt/android/jni_headers/CobaltTextToSpeechHelper_jni.h"

namespace starboard::android::shared {

extern "C" SB_EXPORT_PLATFORM void
JNI_CobaltTextToSpeechHelper_SendTTSChangedEvent(JNIEnv* env) {
  // TODO: (cobalt b/392178584) clean up speech synthesis code, investigate if
  // this is still needed. ApplicationAndroid::Get()->SendTTSChangedEvent();
}

}  // namespace starboard::android::shared
