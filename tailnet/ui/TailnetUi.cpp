#include "TailnetUi.h"

#include <memory>

#include "Tailnet.h"
#include "TailnetLoginActivity.h"
#include "activities/ActivityManager.h"

extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

// Called from CrossInk's main loop (the only task allowed to change
// activities). Shows the login screen once per WiFi session when the tailnet
// asks for a login, never over a book or an exclusive storage operation.
void tailnet_ui_poll() {
  static bool offeredThisSession = false;
  static uint32_t lastGen = 0;

  const uint32_t gen = tailnet_generation();
  if (gen == lastGen) return;  // cheap path: nothing changed
  lastGen = gen;

  const tailnet_status_t st = tailnet_status();
  if (st == TAILNET_OFF) {
    offeredThisSession = false;
    return;
  }
  if (st != TAILNET_NEEDS_LOGIN || offeredThisSession) return;
  if (activityManager.isReaderActivity() || activityManager.requiresExclusiveStorageLoop() ||
      activityManager.hasActivityNamed(TailnetLoginActivity::NAME)) {
    lastGen = gen - 1;  // try again next loop
    return;
  }
  offeredThisSession = true;
  activityManager.pushActivity(std::make_unique<TailnetLoginActivity>(renderer, mappedInputManager));
}
