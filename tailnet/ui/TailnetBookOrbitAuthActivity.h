#pragma once

#include <string>

#include "activities/Activity.h"

// Settings > BookOrbit > Authenticate on the C3 readers (lite builds) when
// Tailscale is on: CrossInk runs it in the normal UI, where the heap cannot
// hold Tailscale too. This runs it in a network boot of its own
// (TAILNET_BOOKORBIT_AUTH_TARGET) instead: "Authenticating..." is drawn, the
// framebuffer is parked while WiFi, Tailscale and the TLS request run, and the
// result is drawn once WiFi is off again. Both buttons then restart to Home,
// as CrossInk's own BookOrbitAuthActivity does after WiFi has been up.
class TailnetBookOrbitAuthActivity final : public Activity {
 public:
  TailnetBookOrbitAuthActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("TailnetBookOrbitAuth", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return state == State::Working; }

 private:
  enum class State { Working, Success, Failed };

  State state = State::Working;
  bool runPending = true;
  std::string errorMessage;

  void run();
};
