// The overlay's only UI hook. CrossInk's main loop calls tailnet_ui_poll() once
// per iteration (overlay/patches/0001-tailnet-ui-hook.patch); it shows the
// Tailscale login screen when the reader needs signing in to the tailnet.
#pragma once

void tailnet_ui_poll();
