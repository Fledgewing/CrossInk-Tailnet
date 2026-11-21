#pragma once

#include <string>

#include "activities/Activity.h"

// Tailscale login on the C3 readers (lite builds), in its own network boot
// (TAILNET_LOGIN_TARGET), so the screen and the network never need memory at
// the same time:
//   1. With the framebuffer parked (the panel keeps its image): WiFi on, one
//      register-only control session that returns the login URL, WiFi off.
//   2. The QR code, drawn with the network off. The user approves the reader
//      on a phone, then presses Confirm.
//   3. Framebuffer parked again (the QR stays on the panel), WiFi on, one more
//      register-only session with the same node key: authorized now, or not
//      yet (the QR again). WiFi off, then the result.
// Back leaves through a silent restart to Home.
class TailnetLoginActivity final : public Activity {
 public:
  TailnetLoginActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TailnetLogin", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // The user is away approving the reader on another device.
  bool preventAutoSleep() override { return true; }

 private:
  enum class State { Working, ShowQr, NotApproved, LoggedIn, NoWifi, NoAnswer };

  State state = State::Working;
  bool runPending = true;
  bool checking = false;  // the session after the QR: the panel keeps showing it
  std::string url;

  void runSession();
};
