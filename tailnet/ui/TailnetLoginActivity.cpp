#include "TailnetLoginActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <memory>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "Tailnet.h"
#include "TailnetNetPhase.h"
#include "activities/ActivityManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/SavedWifiConnect.h"
#include "util/QrUtils.h"

extern GfxRenderer renderer;                    // main.cpp
extern MappedInputManager mappedInputManager;  // main.cpp

namespace {
// Join WiFi, start MicroLink, one control session: ~10-20 s on a good network.
constexpr uint32_t SESSION_TIMEOUT_MS = 45000;
// The shim stops and frees MicroLink after WiFi goes down, then the
// framebuffer comes back (tailnet_display_unpark()).
constexpr uint32_t FRAMEBUFFER_BACK_MS = 10000;
}  // namespace

extern "C" bool tailnet_login_boot(void) {
  auto activity = std::make_unique<TailnetLoginActivity>(renderer, mappedInputManager);
  activityManager.replaceActivity(std::move(activity));
  return true;
}

void TailnetLoginActivity::onEnter() {
  Activity::onEnter();
  state = State::Working;
  runPending = true;
  checking = false;
}

void TailnetLoginActivity::runSession() {
  LOG_INF("TNET", "Login session (%s): WiFi on, free=%u maxAlloc=%u", checking ? "check approval" : "get URL",
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  const bool wifi = SavedWifiConnect::connect([] { return false; });
  tailnet_status_t st = TAILNET_OFF;
  char buf[TAILNET_LOGIN_URL_MAX] = {};
  if (wifi) {
    st = tailnet_login_session(SESSION_TIMEOUT_MS);
    if (st == TAILNET_NEEDS_LOGIN && tailnet_login_url(buf, sizeof(buf))) url = buf;
  }
  SavedWifiConnect::disconnect();
  if (!tailnet_display_ready(FRAMEBUFFER_BACK_MS)) {
    LOG_ERR("TNET", "Login: framebuffer did not come back; restarting");
    silentRestart();
    return;
  }
  // After a restart the check's URL comes from NVS (the shim keeps it there).
  if (url.empty() && checking && tailnet_saved_login_url(buf, sizeof(buf))) url = buf;

  if (!wifi) {
    state = State::NoWifi;
  } else if (st == TAILNET_CONNECTED) {
    state = State::LoggedIn;
  } else if ((st == TAILNET_NEEDS_LOGIN || st == TAILNET_FAILED) && !url.empty()) {
    // FAILED: MachineAuthorized=false without a new URL (e.g. device approval
    // still pending in the admin console): the same QR still applies.
    state = checking ? State::NotApproved : State::ShowQr;
  } else {
    state = State::NoAnswer;
  }
  LOG_INF("TNET", "Login session over: tailnet status %d -> screen %d, free=%u maxAlloc=%u", static_cast<int>(st),
          static_cast<int>(state), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  checking = false;
  requestUpdate(true);
}

void TailnetLoginActivity::loop() {
  if (runPending) {
    runPending = false;
    // First session: draw "Getting a login link" while the framebuffer is
    // still here. A check after the QR leaves the QR on the panel instead.
    if (!checking) requestUpdateAndWait();
    runSession();
    return;
  }
  if (state == State::Working) return;
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    silentRestart();
    return;
  }
  if (!mappedInput.wasReleased(MappedInputManager::Button::Confirm)) return;
  switch (state) {
    case State::ShowQr:
    case State::NotApproved:
      checking = true;
      runPending = true;
      break;
    case State::NoWifi:
    case State::NoAnswer:
      state = State::Working;
      runPending = true;
      break;
    default:
      silentRestart();
      break;
  }
}

void TailnetLoginActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, "Tailscale", false);
  } else {
    GUI.drawHeader(renderer, header, "Tailscale");
  }
  const int top = header.y + header.height + metrics.verticalSpacing;
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int midY = pageHeight / 2;
  const char* confirm = "";

  switch (state) {
    case State::Working:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - lineH, "Getting a login link", true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 8, "from Tailscale...");
      break;
    case State::ShowQr:
    case State::NotApproved: {
      const bool again = state == State::NotApproved;
      renderer.drawCenteredText(UI_12_FONT_ID, top, again ? "Not approved yet" : "Log in to your tailnet", true,
                                EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, top + lineH + 8, "Scan, approve this reader, then press Confirm");
      renderer.drawCenteredText(SMALL_FONT_ID, top + 2 * lineH + 8, "The check takes up to a minute");
      const int qrTop = top + 3 * lineH + 16;
      const int qrBottom = pageHeight - metrics.buttonHintsHeight - 2 * lineH;
      QrUtils::drawQrCode(renderer, Rect(20, qrTop, pageWidth - 40, qrBottom - qrTop), url.c_str());
      renderer.drawCenteredText(SMALL_FONT_ID, qrBottom + lineH / 2, url.c_str());  // to type on a computer
      confirm = "Check";
      break;
    }
    case State::LoggedIn:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - lineH, "Logged in", true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 8, "Turn off key expiry for it in the admin console");
      confirm = tr(STR_DONE);
      break;
    case State::NoWifi:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - lineH, "No saved WiFi network answered", true,
                                EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 8, "Join one in Settings first");
      confirm = tr(STR_RETRY);
      break;
    case State::NoAnswer:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - lineH, "Tailscale did not answer", true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 8, tr(STR_CHECK_SERIAL_OUTPUT));
      confirm = tr(STR_RETRY);
      break;
  }
  if (state != State::Working) {
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), confirm, "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}
