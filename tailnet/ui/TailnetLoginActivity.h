#pragma once

#include <cstdint>
#include <string>

#include "activities/Activity.h"

// Tailscale sign-in: the login URL as a QR code to scan with a phone that is
// signed in to the tailnet. Redraws as the status changes and shows the
// reader's tailnet address once connected. Back closes it.
class TailnetLoginActivity final : public Activity {
 public:
  static constexpr const char* NAME = "TailnetLogin";
  TailnetLoginActivity(GfxRenderer& renderer, MappedInputManager& mappedInput) : Activity(NAME, renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Waiting for a login on another device can take a while.
  bool preventAutoSleep() override { return true; }

 private:
  uint32_t shownGeneration = 0;
  std::string url;
};
