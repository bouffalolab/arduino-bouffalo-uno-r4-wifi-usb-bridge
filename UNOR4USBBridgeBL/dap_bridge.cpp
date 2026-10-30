#include "Arduino.h"
#include "usb_hid_port.h"
#include "dap_bridge.h"
#include "bridge_version.h"
extern "C" {
#include "freedap.h"
}

/* Official bridge's vendor page, 64-byte input/output/feature, no report ID. */
static const uint8_t report_descriptor[] = {
    0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01,
    0x09, 0x02, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x40, 0x81, 0x02,
    0x09, 0x03, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x40, 0x91, 0x02,
    0x09, 0x04, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x40, 0xb1, 0x02, 0xc0
};

static volatile bool boot_requested;

/**
 * @brief Copy the bridge version into the CMSIS-DAP HID Feature response.
 */
static uint16_t get_feature(uint8_t *data)
{
    data[0] = BRIDGE_VERSION_MAJOR;
    data[1] = BRIDGE_VERSION_MINOR;
    data[2] = BRIDGE_VERSION_PATCH;
    return 3;
}

/**
 * @brief Handle the supported CMSIS-DAP HID Feature command.
 */
static void set_feature(const uint8_t *data, uint16_t size)
{
    if (size > 0 && data[0] == 0xaa) {
        boot_requested = true;
    }
}

static const usb_hid_port_config hid_config = {
    report_descriptor, sizeof(report_descriptor), get_feature, set_feature
};

/* Core starts USB before setup(); register the optional interface beforehand. */
static struct RegisterHID {
    /**
     * @brief Register the bridge-owned CMSIS-DAP HID interface before USB initialization.
     */
    RegisterHID() { usb_hid_port_register(&hid_config); }
} register_hid;

/**
 * @brief Initialize the bridge's Free-DAP state for the RA4M1 SWD target.
 */
void dap_bridge_begin()
{
    dap_init();
}

/**
 * @brief Release the SWD connection while retaining bridge reset ownership.
 */
void dap_bridge_disconnect()
{
    uint8_t request[] = { 0x03 }; // DAP_Disconnect releases SWD pins and state.
    uint8_t response[2];
    dap_process_request(request, sizeof(request), response, sizeof(response));
}

/**
 * @brief Process a pending CMSIS-DAP HID request and submit its response.
 */
void dap_bridge_poll()
{
    if (boot_requested) {
        delay(100); // Finish SET_REPORT status stage before resetting USB.
        resetToBootloader();
    }
    uint8_t request[USB_HID_PORT_PACKET_SIZE];
    int size = usb_hid_port_read(request);
    if (size > 0) {
        uint8_t response[USB_HID_PORT_PACKET_SIZE] = {};
        dap_process_request(request, size, response, sizeof(response));
        usb_hid_port_write(response);
    }
}
