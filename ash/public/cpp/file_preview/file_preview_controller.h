// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_PUBLIC_CPP_FILE_PREVIEW_FILE_PREVIEW_CONTROLLER_H_
#define ASH_PUBLIC_CPP_FILE_PREVIEW_FILE_PREVIEW_CONTROLLER_H_

#include "ash/public/cpp/ash_public_export.h"
#include "ash/public/cpp/file_preview/file_preview_image_skia_source.h"
#include "base/callback_list.h"
#include "ui/gfx/image/image_skia.h"

namespace base {
class FilePath;
}

namespace gfx {
class Size;
}

namespace ash {

// Enables control over the behavior of a `ui::ImageModel` wrapping a
// `FilePreviewImageSkiaSource`. Copies of an image model will have the same
// `FilePreviewController`.
class ASH_PUBLIC_EXPORT FilePreviewController {
 public:
  using Key = const void*;
  using PlaybackMode = FilePreviewImageSkiaSource::PlaybackMode;

  FilePreviewController(base::FilePath path, const gfx::Size& size);
  FilePreviewController(const FilePreviewController&) = delete;
  FilePreviewController& operator=(const FilePreviewController&) = delete;
  ~FilePreviewController();

  // Fetches the `Key` used to map a given `gfx::ImageSkia` to its
  // `FilePreviewController`. Currently only used to check for regression.
  static Key GetKey(const gfx::ImageSkia& image_skia);

  // Adds a closure to be called whenever the image is invalidated, e.g., when
  // it should be redrawn.
  [[nodiscard]] base::CallbackListSubscription AddInvalidationCallback(
      base::RepeatingClosure callback);

  // Sets the current behavior for animation, and begin animating if
  // appropriate. No-op if `playback_mode` is the same as the current value.
  // TODO(http://b/277763997): Consider changing this interface to instead
  // consist of a method that provides a scoped animation object.
  void SetPlaybackMode(PlaybackMode playback_mode);

  // Only called by `FilePreviewImageSkiaSource` when a new image is ready to
  // show. Not callable by anyone else to prevent infinite loops and excessive
  // repainting of views.
  void Invalidate(base::PassKey<FilePreviewImageSkiaSource>);

  // TODO(http://b/266000869): Remove once `FilePreviewFactory` is implemented
  // and producing working `ui::ImageModel`s.
  const gfx::ImageSkia& GetImageSkiaForTest() const;

 private:
  // The `FilePreviewImageSkiaSource` this controller controls. Owned by the
  // `gfx::ImageSkia`.
  const base::raw_ptr<FilePreviewImageSkiaSource> source_ = nullptr;

  // The `gfx::ImageSkia` that contains the `FilePreviewImageSkiaSource`. This
  // is the same one in the `ui::ImageModel` that `FilePreviewFactory` creates.
  // When the image has changed, invalidation will remove the reps to cause all
  // repaints to fetch the new image from `FilePreviewImageSkiaSource` instead
  // of using the a cached rep.
  gfx::ImageSkia image_skia_;

  // The `Key` used to identify the `image_skia_`. Saved here to check for
  // regressions.
  const Key key_ = nullptr;

  // All callbacks submitted by callers to listen for invalidation.
  base::RepeatingClosureList invalidation_callbacks_;
};

}  // namespace ash

#endif  // ASH_PUBLIC_CPP_FILE_PREVIEW_FILE_PREVIEW_CONTROLLER_H_
