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

#include "cobalt/media/audio/cobalt_audio_device_factory.h"

#include "base/logging.h"
#include "cobalt/media/audio/audio_helpers.h"
#include "cobalt/media/audio/cobalt_audio_renderer_sink.h"

namespace media {

CobaltAudioDeviceFactory::CobaltAudioDeviceFactory() {
  LOG(INFO) << "Register CobaltAudioDeviceFactory";
}

CobaltAudioDeviceFactory::~CobaltAudioDeviceFactory() {
  LOG(INFO) << "Unregister CobaltAudioDeviceFactory";
}

scoped_refptr<media::AudioRendererSink>
CobaltAudioDeviceFactory::NewAudioRendererSink(
    blink::WebAudioDeviceSourceType source_type,
    const blink::LocalFrameToken& frame_token,
    const media::AudioSinkParameters& params) {
  return base::MakeRefCounted<media::CobaltAudioRendererSink>();
}

OutputDeviceInfo CobaltAudioDeviceFactory::GetOutputDeviceInfo(
    const blink::LocalFrameToken& frame_token,
    const std::string& device_id) {
  return GetPreferredOutputParameters();
}
}  // namespace media
