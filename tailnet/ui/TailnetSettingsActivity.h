#pragma once

#include <cstdint>
#include <string>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Settings > Tailscale. Shows the tailnet status and address, and offers:
//   Tailscale  On/Off   persisted switch (off by default)
//   Log in / Log out    join this reader to a tailnet with a QR code, or forget
//                       its tailnet identity
//   Diagnostics         peer table, the server last reached, recent log lines
// Logging in turns WiFi on for the screen if it is off (the same way
// ClockSyncActivity does) and off again on exit.
class TailnetSettingsActivity final : public Activity {
 public:
  static constexpr const char* NAME = "TailnetSettings";
  TailnetSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity(NAME, renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Waiting for a login on another device can take a while.
  bool preventAutoSleep() override { return mode == Mode::LOGIN; }

 private:
  enum class Mode { MENU, LOGIN, DIAG };
  enum Row { ROW_ENABLED = 0, ROW_ACCOUNT, ROW_DIAG, ROW_COUNT };

  Mode mode = Mode::MENU;
  int selected = 0;
  uint32_t shownGeneration = 0;
  bool tearDownWifiOnExit = false;
  bool loggedOutNotice = false;
  ButtonNavigator buttonNavigator;
  int listTop = 0;
  uint32_t shownLogSeq = 0;
  unsigned long lastDiagRender = 0;  // y of the first row, from the last render (for taps)

  void activate(int row);
  void startLogin();
  void renderMenu(int top);
  void renderLogin(int top);
  void renderDiag(int top);
};
