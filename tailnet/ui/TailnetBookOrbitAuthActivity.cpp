#include "TailnetBookOrbitAuthActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>

#include <memory>

#include "BookOrbitSyncClient.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "Tailnet.h"
#include "TailnetNetPhase.h"
#include "activities/ActivityManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/SavedWifiConnect.h"

extern GfxRenderer renderer;                    // main.cpp
extern MappedInputManager mappedInputManager;  // main.cpp

namespace {
// The shim stops and frees MicroLink after WiFi goes down, then the
// framebuffer comes back (tailnet_display_unpark()).
constexpr uint32_t FRAMEBUFFER_BACK_MS = 10000;
}  // namespace

extern "C" bool tailnet_redirect_bookorbit_auth(void) {
  // Only when the request has to go through Tailscale; otherwise CrossInk's
  // own screen works in the normal UI as before.
  if (!tailnet_lite() || !tailnet_enabled() || !tailnet_joined()) return false;
  LOG_INF("TNET", "BookOrbit authentication: restarting into its network boot");
  silentRestartToNetwork(static_cast<NetworkBootTarget>(TAILNET_BOOKORBIT_AUTH_TARGET));
  return true;
}

extern "C" bool tailnet_bookorbit_auth_boot(void) {
  auto activity = std::make_unique<TailnetBookOrbitAuthActivity>(renderer, mappedInputManager);
  activityManager.replaceActivity(std::move(activity));
  return true;
}

void TailnetBookOrbitAuthActivity::onEnter() {
  Activity::onEnter();
  state = State::Working;
  runPending = true;
}

void TailnetBookOrbitAuthActivity::run() {
  sdFontSystem.releaseLoadedFont(renderer);
  // "Authenticating..." goes on the panel while the framebuffer is still here.
  requestUpdateAndWait();
  sdFontSystem.releaseForNetwork(renderer);
  LOG_INF("TNET", "BookOrbit authentication: WiFi on, free=%u maxAlloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  const bool wifi = SavedWifiConnect::connect([] { return false; });
  auto result = BookOrbitSyncClient::NETWORK_ERROR;
  if (wifi) {
#ifndef SIMULATOR
    halClock.syncSystemTimeFromNTP();  // certificate dates; WifiSelectionActivity does the same
#endif
    result = BookOrbitSyncClient::authenticate();
  }
  LOG_INF("TNET", "BookOrbit authentication: result %d (tailnet status %d), free=%u maxAlloc=%u",
          static_cast<int>(result), static_cast<int>(tailnet_status()), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  tailnet_wait_idle(10000);  // never cut a control session off mid-exchange
  SavedWifiConnect::disconnect();
  if (!tailnet_display_ready(FRAMEBUFFER_BACK_MS)) {
    LOG_ERR("TNET", "BookOrbit authentication: framebuffer did not come back; restarting");
    silentRestart();
    return;
  }
  if (!wifi) {
    state = State::Failed;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
  } else if (result == BookOrbitSyncClient::OK) {
    state = State::Success;
  } else {
    state = State::Failed;
    errorMessage = BookOrbitSyncClient::errorString(result);
  }
  requestUpdate(true);
}

void TailnetBookOrbitAuthActivity::loop() {
  if (runPending) {
    runPending = false;
    run();
    return;
  }
  if (state == State::Working) return;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    silentRestart();
  }
}

void TailnetBookOrbitAuthActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto pageHeight = renderer.getScreenHeight();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, tr(STR_BOOKORBIT_AUTH), /*readerContext=*/false);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_BOOKORBIT_AUTH));
  }
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;
  switch (state) {
    case State::Working:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_AUTHENTICATING));
      break;
    case State::Success:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_AUTH_SUCCESS), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, tr(STR_BOOKORBIT_SYNC_READY));
      break;
    case State::Failed:
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_AUTH_FAILED), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, top + height + 10, errorMessage.c_str());
      break;
  }
  if (state != State::Working) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}
