#include "TailnetSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "Tailnet.h"
#include "TailnetNetPhase.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace {

constexpr const char* TITLE = "Tailscale";
constexpr const char* HELP_URL = "https://github.com/Fledgewing/CrossInk-Tailnet#recommended-tailnet-setup";

std::string formatIp(uint32_t ip) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", (unsigned)(ip >> 24) & 0xff, (unsigned)(ip >> 16) & 0xff,
           (unsigned)(ip >> 8) & 0xff, (unsigned)ip & 0xff);
  return buf;
}

// One line describing where the reader stands.
std::string statusLine() {
  if (!tailnet_enabled()) return "Off";
  switch (tailnet_status()) {
    case TAILNET_CONNECTED:
      return "Connected as " + formatIp(tailnet_ip());
    case TAILNET_STARTING:
    case TAILNET_RECONNECTING:
      return "Connecting...";
    case TAILNET_NEEDS_LOGIN:
      return "Not logged in";
    case TAILNET_FAILED:
      return "Could not connect (see serial log)";
    case TAILNET_OFF:
    default:
      if (!tailnet_joined()) return "On; not logged in";
      return tailnet_lite() ? "On; connects for BookOrbit syncs" : "On; connects when WiFi is on";
  }
}

}  // namespace

void TailnetSettingsActivity::onEnter() {
  Activity::onEnter();
  mode = Mode::MENU;
  shownGeneration = tailnet_generation();
  requestUpdate();
}

void TailnetSettingsActivity::onExit() {
  Activity::onExit();
  if (tearDownWifiOnExit && WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    WiFi.mode(WIFI_OFF);
  }
}

void TailnetSettingsActivity::startLogin() {
  loggedOutNotice = false;
  if (!tailnet_enabled()) tailnet_set_enabled(true);
  if (tailnet_lite()) {
    // The C3 cannot hold this screen and Tailscale at once: log in from a
    // network boot of its own (TailnetLoginActivity).
    silentRestartToNetwork(static_cast<NetworkBootTarget>(TAILNET_LOGIN_TARGET));
    return;
  }
  mode = Mode::LOGIN;
  if (WiFi.status() == WL_CONNECTED) {
    requestUpdate();
    return;
  }
  tearDownWifiOnExit = true;
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) mode = Mode::MENU;
                           requestUpdate();
                         });
}

void TailnetSettingsActivity::activate(const int row) {
  switch (row) {
    case ROW_ENABLED:
      loggedOutNotice = false;
      tailnet_set_enabled(!tailnet_enabled());
      break;
    case ROW_DIAG:
      mode = Mode::DIAG;
      break;
    case ROW_HELP:
      mode = Mode::HELP;
      break;
    case ROW_RELAY:
      openRelayList();
      break;
    case ROW_ACCOUNT:
      if (tailnet_joined()) {
        // Logging out forgets this reader's Tailscale identity: getting back
        // needs a new QR login (and re-tagging it in the admin console).
        startActivityForResult(
            std::make_unique<ConfirmationActivity>(
                renderer, mappedInput, "Log out of Tailscale?",
                "This reader leaves your tailnet. To reconnect you'll need to log in again with the QR code."),
            [this](const ActivityResult& result) {
              if (!result.isCancelled) {
                tailnet_logout();
                loggedOutNotice = true;
              }
              requestUpdate();
            });
        return;
      } else {
        startLogin();
        return;
      }
      break;
    default:
      return;
  }
  requestUpdate();
}

void TailnetSettingsActivity::loop() {
  const bool back = TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
                    mappedInput.wasReleased(MappedInputManager::Button::Back);
  if (mode == Mode::RELAY) {
    loopRelayList(back);
  } else if (mode != Mode::MENU) {
    if (back) {
      mode = Mode::MENU;
      requestUpdate();
      return;
    }
  } else {
    if (back) {
      finish();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activate(selected);
      return;
    }
    int x = 0;
    int y = 0;
    if (mappedInput.wasScreenTapped(x, y)) {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const int rowH = metrics.listRowHeight + metrics.listRowGap;
      if (y >= listTop && rowH > 0) {
        const int row = (y - listTop) / rowH;
        if (row >= 0 && row < ROW_COUNT) {
          selected = row;
          activate(row);
        }
      }
      return;
    }
    buttonNavigator.onNextRelease([this] {
      selected = ButtonNavigator::nextIndex(selected, ROW_COUNT);
      requestUpdate();
    });
    buttonNavigator.onPreviousRelease([this] {
      selected = ButtonNavigator::previousIndex(selected, ROW_COUNT);
      requestUpdate();
    });
  }

  // Redraw on tailnet changes (status, login URL, address), and on the
  // diagnostics page for new log lines, at most every 3 s (e-ink).
  const uint32_t gen = tailnet_generation();
  if (gen != shownGeneration) {
    shownGeneration = gen;
    requestUpdate();
  } else if (mode == Mode::DIAG && tailnet_log_seq() != shownLogSeq && millis() - lastDiagRender >= 3000) {
    requestUpdate();
  }
}

void TailnetSettingsActivity::renderMenu(const int top) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);

  renderer.drawCenteredText(UI_10_FONT_ID, top, statusLine().c_str());
  listTop = top + lineH + metrics.verticalSpacing;

  const bool joined = tailnet_joined();
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, ROW_COUNT * (metrics.listRowHeight + metrics.listRowGap)}, ROW_COUNT,
      selected,
      [joined](int i) -> std::string {
        if (i == ROW_ENABLED) return "Tailscale";
        if (i == ROW_DIAG) return "Diagnostics";
        if (i == ROW_HELP) return "Setup help";
        if (i == ROW_RELAY) return "Relay region";
        return joined ? "Log out" : "Log in";
      },
      nullptr, nullptr,
      [this](int i) -> std::string {
        if (i == ROW_ENABLED) return tailnet_enabled() ? tr(STR_ON) : tr(STR_OFF);
        if (i == ROW_RELAY) return relayValue();
        return "";
      });

  const int noteY = listTop + ROW_COUNT * (metrics.listRowHeight + metrics.listRowGap) + metrics.verticalSpacing;
  tailnet_diag_t diag;
  tailnet_get_diag(&diag);
  const bool tableFull = diag.peer_capacity > 0 && diag.peer_count >= diag.peer_capacity;
  if (tableFull) {
    // The tailnet sends this reader more devices than it can track: the
    // server may be left out. Tagging the reader fixes it (Setup help).
    renderer.drawCenteredText(UI_10_FONT_ID, noteY, "Peer list full: tagging this reader", true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, noteY + lineH, "keeps it short (see Setup help)");
  } else if (loggedOutNotice) {
    renderer.drawCenteredText(UI_10_FONT_ID, noteY, "Logged out. Remove the old device");
    renderer.drawCenteredText(UI_10_FONT_ID, noteY + lineH, "in the Tailscale admin console.");
  } else if (joined) {
    renderer.drawCenteredText(SMALL_FONT_ID, noteY, "Turn off key expiry for this reader");
    renderer.drawCenteredText(SMALL_FONT_ID, noteY + lineH, "in the Tailscale admin console.");
  }
}

void TailnetSettingsActivity::renderLogin(const int top) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int midY = pageHeight / 2;

  char url[TAILNET_LOGIN_URL_MAX];
  switch (tailnet_status()) {
    case TAILNET_NEEDS_LOGIN:
      if (tailnet_login_url(url, sizeof(url))) {
        renderer.drawCenteredText(UI_12_FONT_ID, top, "Log in to your tailnet", true, EpdFontFamily::BOLD);
        renderer.drawCenteredText(UI_10_FONT_ID, top + lineH + 8, "Scan with a phone signed in to Tailscale");
        const int qrTop = top + 2 * lineH + 24;
        const int qrBottom = pageHeight - metrics.buttonHintsHeight - 3 * lineH;
        QrUtils::drawQrCode(renderer, Rect(20, qrTop, pageWidth - 40, qrBottom - qrTop), url);
        renderer.drawCenteredText(SMALL_FONT_ID, qrBottom + lineH / 2, url);  // to type on a computer
        break;
      }
      renderer.drawCenteredText(UI_12_FONT_ID, midY, "Waiting for the login link...");
      break;
    case TAILNET_CONNECTED:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - lineH, "On your tailnet", true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 8, ("Address " + formatIp(tailnet_ip())).c_str());
      renderer.drawCenteredText(UI_10_FONT_ID, midY + lineH + 16, "Turn off key expiry for it in the admin console");
      break;
    case TAILNET_FAILED:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - lineH, "Tailscale could not connect", true,
                                EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 8, tr(STR_CHECK_SERIAL_OUTPUT));
      break;
    default:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, "Connecting to Tailscale...");
      break;
  }
}

void TailnetSettingsActivity::renderDiag(const int top) {
  shownLogSeq = tailnet_log_seq();
  lastDiagRender = millis();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageHeight = renderer.getScreenHeight();
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int smallH = renderer.getLineHeight(SMALL_FONT_ID);
  const int x = 12;
  int y = top;

  tailnet_diag_t d;
  tailnet_get_diag(&d);
  char line[96];
  renderer.drawText(UI_10_FONT_ID, x, y, statusLine().c_str());
  y += lineH;
  snprintf(line, sizeof(line), "Peers tracked: %d", d.peer_count);
  renderer.drawText(UI_10_FONT_ID, x, y, line);
  y += lineH;
  if (d.target_ip) {
    snprintf(line, sizeof(line), "Server %s: %s", formatIp(d.target_ip).c_str(),
             !d.target_known    ? "NOT a peer"
             : !d.target_online ? "peer, offline"
             : d.target_direct  ? "peer, direct"
                                : "peer, via relay");
    renderer.drawText(UI_10_FONT_ID, x, y, line);
    y += lineH;
  }
  if (d.home_relay[0]) {
    snprintf(line, sizeof(line), "Relay: ours %s, server %s", d.home_relay, d.target_relay[0] ? d.target_relay : "?");
    renderer.drawText(UI_10_FONT_ID, x, y, line);
    y += lineH;
  }
  if (d.pin_ip) {
    snprintf(line, sizeof(line), "Pinned: %s", formatIp(d.pin_ip).c_str());
    renderer.drawText(UI_10_FONT_ID, x, y, line);
    y += lineH;
  }
  y += metrics.verticalSpacing;

  // Recent log, newest first, as many lines as fit.
  const int bottom = pageHeight - metrics.buttonHintsHeight - 4;
  const int maxWidth = renderer.getScreenWidth() - 2 * x;
  char log[128];
  for (int i = 0; y + smallH <= bottom && tailnet_log_line(i, log, sizeof(log)); i++) {
    // Trim to the screen width.
    size_t n = strlen(log);
    while (n > 4 && renderer.getTextWidth(SMALL_FONT_ID, log) > maxWidth) log[--n] = '\0';
    renderer.drawText(SMALL_FONT_ID, x, y, log);
    y += smallH;
  }
}

void TailnetSettingsActivity::renderHelp(const int top) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int x = 12;
  int y = top;

  renderer.drawText(UI_12_FONT_ID, x, y, "Recommended tailnet setup", true, EpdFontFamily::BOLD);
  y += renderer.getLineHeight(UI_12_FONT_ID) + 4;
  // Short lines: they must fit the narrowest reader at UI_10.
  static const char* const kLines[] = {
      "Let this reader reach only your",
      "BookOrbit server: a lost reader",
      "then can't reach your tailnet, its",
      "key never expires, and its device",
      "list stays short.",
      "",
      "1. Access controls: add tag:ereader",
      "   and a grant from tag:ereader to",
      "   the server's 100.x address.",
      "2. Machines > this reader >",
      "   Edit ACL tags > tag:ereader",
      "3. Avoid rules with dst \"*\", and",
      "   don't give it Mullvad access.",
      "",
      "Full steps and a policy example:",
  };
  for (const char* line : kLines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line);
    y += lineH;
  }
  const int qrBottom = pageHeight - metrics.buttonHintsHeight - 8;
  if (qrBottom - y > 80) QrUtils::drawQrCode(renderer, Rect(20, y + 4, pageWidth - 40, qrBottom - y - 4), HELP_URL);
}

std::string TailnetSettingsActivity::relayValue() const {
  switch (tailnet_relay_mode()) {
    case TAILNET_RELAY_MATCH_SERVER:
      return "Match server";
    case TAILNET_RELAY_FIXED: {
      char code[16];
      tailnet_relay_code(tailnet_relay_fixed_region(), code, sizeof(code));
      return code;
    }
    default:
      return "Automatic";
  }
}

void TailnetSettingsActivity::openRelayList() {
  relayRegionCount = tailnet_relay_regions(relayIds, relayCodes, MAX_REGIONS);
  // Not connected yet: no map. Still offer the region already chosen.
  const uint16_t fixed = tailnet_relay_fixed_region();
  if (relayRegionCount == 0 && fixed) {
    relayIds[0] = fixed;
    tailnet_relay_code(fixed, relayCodes[0], sizeof(relayCodes[0]));
    relayRegionCount = 1;
  }
  relaySelected = 0;
  switch (tailnet_relay_mode()) {
    case TAILNET_RELAY_MATCH_SERVER:
      relaySelected = 1;
      break;
    case TAILNET_RELAY_FIXED:
      for (int i = 0; i < relayRegionCount; i++) {
        if (relayIds[i] == fixed) relaySelected = 2 + i;
      }
      break;
    default:
      break;
  }
  mode = Mode::RELAY;
}

void TailnetSettingsActivity::chooseRelay(const int index) {
  if (index == 0) {
    tailnet_set_relay(TAILNET_RELAY_AUTO, 0);
  } else if (index == 1) {
    tailnet_set_relay(TAILNET_RELAY_MATCH_SERVER, 0);
  } else if (index - 2 < relayRegionCount) {
    tailnet_set_relay(TAILNET_RELAY_FIXED, relayIds[index - 2]);
  }
  mode = Mode::MENU;
  requestUpdate();
}

void TailnetSettingsActivity::loopRelayList(const bool back) {
  const int count = 2 + relayRegionCount;
  if (back) {
    mode = Mode::MENU;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    chooseRelay(relaySelected);
    return;
  }
  int x = 0;
  int y = 0;
  if (mappedInput.wasScreenTapped(x, y)) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int rowH = metrics.listRowHeight + metrics.listRowGap;
    const int listH = renderer.getScreenHeight() - listTop - metrics.buttonHintsHeight;
    const int pageItems = rowH > 0 ? std::max(1, listH / metrics.listRowHeight) : 1;
    if (y >= listTop && rowH > 0) {
      const int index = (relaySelected / pageItems) * pageItems + (y - listTop) / rowH;
      if (index >= 0 && index < count) chooseRelay(index);
    }
    return;
  }
  buttonNavigator.onNextRelease([this, count] {
    relaySelected = ButtonNavigator::nextIndex(relaySelected, count);
    requestUpdate();
  });
  buttonNavigator.onPreviousRelease([this, count] {
    relaySelected = ButtonNavigator::previousIndex(relaySelected, count);
    requestUpdate();
  });
}

void TailnetSettingsActivity::renderRelay(const int top) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);

  renderer.drawCenteredText(SMALL_FONT_ID, top, "Where relayed traffic meets the tailnet");
  renderer.drawCenteredText(SMALL_FONT_ID, top + lineH, relayRegionCount ? "" : "Connect once to list regions");
  listTop = top + 2 * lineH + metrics.verticalSpacing;
  const int listH = renderer.getScreenHeight() - listTop - metrics.buttonHintsHeight;
  const int count = 2 + relayRegionCount;
  const tailnet_relay_mode_t current = tailnet_relay_mode();
  const uint16_t fixed = tailnet_relay_fixed_region();
  GUI.drawList(
      renderer, Rect{0, listTop, pageWidth, listH}, count, relaySelected,
      [this](int i) -> std::string {
        if (i == 0) return "Automatic (closest)";
        if (i == 1) return "Match BookOrbit server";
        return relayCodes[i - 2];
      },
      nullptr, nullptr,
      [this, current, fixed](int i) -> std::string {
        const bool on = (i == 0 && current == TAILNET_RELAY_AUTO) ||
                        (i == 1 && current == TAILNET_RELAY_MATCH_SERVER) ||
                        (i >= 2 && current == TAILNET_RELAY_FIXED && relayIds[i - 2] == fixed);
        return on ? "Current" : "";
      });
}

void TailnetSettingsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, TITLE, false);
  } else {
    GUI.drawHeader(renderer, header, TITLE);
  }
  const int top = header.y + header.height + metrics.verticalSpacing;

  if (mode == Mode::RELAY) {
    renderRelay(top);
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT),
                                              tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (mode == Mode::HELP) {
    renderHelp(top);
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (mode == Mode::DIAG) {
    renderDiag(top);
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else if (mode == Mode::LOGIN) {
    renderLogin(top);
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    renderMenu(top);
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT),
                                              tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}
