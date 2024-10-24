// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/media_router/browser/mirroring_media_controller_host.h"

#include "base/run_loop.h"
#include "base/test/gmock_callback_support.h"
#include "base/test/task_environment.h"
#include "components/media_router/common/media_route.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace media_router {

class MockMirroringMediaControllerHostObserver
    : public MirroringMediaControllerHost::Observer {
 public:
  MockMirroringMediaControllerHostObserver() = default;
  explicit MockMirroringMediaControllerHostObserver(
      MirroringMediaControllerHost* host)
      : host_(host) {
    host_->AddObserver(this);
  }

  ~MockMirroringMediaControllerHostObserver() override {
    if (host_) {
      host_->RemoveObserver(this);
    }
  }

  MOCK_METHOD(void, OnFreezeInfoChanged, (), (override));

 private:
  raw_ptr<MirroringMediaControllerHost> host_ = nullptr;
};

class MockMediaController : public mojom::MediaController {
 public:
  explicit MockMediaController(
      mojo::PendingReceiver<mojom::MediaController> receiver)
      : receiver_(this, std::move(receiver)) {}
  ~MockMediaController() override = default;

  MOCK_METHOD(void, Play, ());
  MOCK_METHOD(void, Pause, ());
  MOCK_METHOD(void, SetMute, (bool mute));
  MOCK_METHOD(void, SetVolume, (float volume));
  MOCK_METHOD(void, Seek, (base::TimeDelta time));
  MOCK_METHOD(void, NextTrack, ());
  MOCK_METHOD(void, PreviousTrack, ());

  void FlushForTesting() { receiver_.FlushForTesting(); }

 private:
  mojo::Receiver<mojom::MediaController> receiver_;
};

class MirroringMediaControllerHostTest : public ::testing::Test {
 public:
  MirroringMediaControllerHostTest() = default;
  ~MirroringMediaControllerHostTest() override = default;

  void SetUp() override {
    mojo::Remote<mojom::MediaController> controller_remote;
    media_controller_ = std::make_unique<MockMediaController>(
        controller_remote.BindNewPipeAndPassReceiver());

    host_ = std::make_unique<MirroringMediaControllerHost>(
        std::move(controller_remote));
  }

 protected:
  base::test::SingleThreadTaskEnvironment task_environment_;
  std::unique_ptr<MirroringMediaControllerHost> host_;
  std::unique_ptr<MockMediaController> media_controller_;
};

TEST_F(MirroringMediaControllerHostTest, GetMediaStatusObserverPendingRemote) {
  auto observer_remote = host_->GetMediaStatusObserverPendingRemote();
  EXPECT_TRUE(observer_remote.is_valid());
}

TEST_F(MirroringMediaControllerHostTest, OnMediaStatusUpdated) {
  // Constructing will add itself as an observer of |host|.
  MockMirroringMediaControllerHostObserver observer(host_.get());

  EXPECT_CALL(observer, OnFreezeInfoChanged);
  host_->OnMediaStatusUpdated(mojom::MediaStatus::New());
}

TEST_F(MirroringMediaControllerHostTest, OnMediaStatusUpdatedCanFreeze) {
  media_router::mojom::MediaStatusPtr status = mojom::MediaStatus::New();
  status->can_play_pause = true;

  EXPECT_FALSE(host_->can_freeze());
  host_->OnMediaStatusUpdated(std::move(status));
  EXPECT_TRUE(host_->can_freeze());
}

TEST_F(MirroringMediaControllerHostTest, OnMediaStatusUpdatedIsFrozen) {
  media_router::mojom::MediaStatusPtr status = mojom::MediaStatus::New();
  status->play_state = mojom::MediaStatus::PlayState::PAUSED;

  EXPECT_FALSE(host_->is_frozen());
  host_->OnMediaStatusUpdated(status.Clone());
  EXPECT_FALSE(host_->is_frozen());

  status->can_play_pause = true;
  host_->OnMediaStatusUpdated(std::move(status));
  EXPECT_TRUE(host_->is_frozen());
}

TEST_F(MirroringMediaControllerHostTest, Freeze) {
  EXPECT_CALL(*media_controller_, Pause);
  host_->Freeze();
  media_controller_->FlushForTesting();
}

TEST_F(MirroringMediaControllerHostTest, Unfreeze) {
  EXPECT_CALL(*media_controller_, Play);
  host_->Unfreeze();
  media_controller_->FlushForTesting();
}

}  // namespace media_router
