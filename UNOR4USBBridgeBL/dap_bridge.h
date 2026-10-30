#pragma once

/**
 * @brief Initialize the bridge's Free-DAP state for the RA4M1 SWD target.
 */
void dap_bridge_begin();
/**
 * @brief Process a pending CMSIS-DAP HID request and submit its response.
 */
void dap_bridge_poll();
/**
 * @brief Call in the main loop before 1200/2400 changes the target's boot mode.
 */
void dap_bridge_disconnect();
