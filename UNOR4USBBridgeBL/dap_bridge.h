#pragma once

void dap_bridge_begin();
void dap_bridge_poll();
/* Call in the main loop before 1200/2400 changes the target's boot mode. */
void dap_bridge_disconnect();
