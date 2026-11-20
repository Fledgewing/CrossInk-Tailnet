#include "TailnetLoginActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "Tailnet.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace {
constexpr const char* TITLE = "Tailscale";
}

void TailnetLoginActivity::onEnter() {
  Activity::onEnter();
  shownGeneration = tailnet_generation();
  requestUpdate();
}

void TailnetLoginActivity::loop() {
  int x = 0;
  int y = 0;
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back) || mappedInput.wasScreenTapped(x, y)) {
    finish();
    return;
  }
  const uint32_t gen = tailnet_generation();
  if (gen != shownGeneration) {
    shownGeneration = gen;
    requestUpdate();
  }
}

void TailnetLoginActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, TITLE, false);
  } else {
    GUI.drawHeader(renderer, header, TITLE);
  }

  const int top = header.y + header.height + metrics.verticalSpacing;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const tailnet_status_t st = tailnet_status();

  char buf[TAILNET_LOGIN_URL_MAX];
  if (st == TAILNET_NEEDS_LOGIN && tailnet_login_url(buf, sizeof(buf))) {
    url = buf;
    renderer.drawCenteredText(UI_12_FONT_ID, top, "Sign in to your tailnet", true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, top + lineH + 8, "Scan with a phone signed in to Tailscale");
    const int qrTop = top + 2 * lineH + 24;
    const int qrBottom = pageHeight - metrics.buttonHintsHeight - 3 * lineH;
    QrUtils::drawQrCode(renderer, Rect(20, qrTop, pageWidth - 40, qrBottom - qrTop), url);
    // The URL as text too, for typing it in on a computer.
    renderer.drawCenteredText(SMALL_FONT_ID, qrBottom + lineH / 2, url.c_str());
  } else {
    const int midY = pageHeight / 2;
    switch (st) {
      case TAILNET_CONNECTED: {
        const uint32_t ip = tailnet_ip();
        char line[48];
        snprintf(line, sizeof(line), "Connected as %u.%u.%u.%u", (unsigned)(ip >> 24) & 0xff,
                 (unsigned)(ip >> 16) & 0xff, (unsigned)(ip >> 8) & 0xff, (unsigned)ip & 0xff);
        renderer.drawCenteredText(UI_12_FONT_ID, midY - lineH, "On your tailnet", true, EpdFontFamily::BOLD);
        renderer.drawCenteredText(UI_10_FONT_ID, midY + 8, line);
        renderer.drawCenteredText(UI_10_FONT_ID, midY + lineH + 16,
                                  "Turn off key expiry for this reader in the admin console");
        break;
      }
      case TAILNET_FAILED:
        renderer.drawCenteredText(UI_12_FONT_ID, midY - lineH, "Tailscale could not connect", true,
                                  EpdFontFamily::BOLD);
        renderer.drawCenteredText(UI_10_FONT_ID, midY + 8, tr(STR_CHECK_SERIAL_OUTPUT));
        break;
      case TAILNET_OFF:
        renderer.drawCenteredText(UI_12_FONT_ID, midY, "Tailscale is off (no WiFi)");
        break;
      default:
        renderer.drawCenteredText(UI_12_FONT_ID, midY, "Connecting to Tailscale...");
        break;
    }
  }

  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
