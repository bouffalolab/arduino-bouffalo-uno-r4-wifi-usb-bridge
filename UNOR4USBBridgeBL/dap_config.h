/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2017-2022, Alex Taradov <alex@taradov.com>. All rights reserved.
 * BL616CL port of Alex Taradov's Free-DAP. Only SWD is provided.
 */
#pragma once
#include "bflb_gpio.h"
#include "bflb_mtimer.h"
#include "pin_keep.h"

#define DAP_CONFIG_DEFAULT_PORT DAP_PORT_SWD
#define DAP_CONFIG_DEFAULT_CLOCK 100000
#define DAP_CONFIG_PACKET_SIZE 64
#define DAP_CONFIG_PACKET_COUNT 1
#define DAP_CONFIG_PERFORMANCE_ATTR
#define DAP_CONFIG_DELAY_CONSTANT 500
#define DAP_CONFIG_FAST_CLOCK 500000
#define DAP_CONFIG_VENDOR_STR "Bouffalolab"
#define DAP_CONFIG_PRODUCT_STR "BL616CL UNO R4 WiFi CMSIS-DAP"
#define DAP_CONFIG_CMSIS_DAP_VER_STR "2.0.0"

#if defined(BRIDGE_MODULE_1V1)
#define DAP_SWCLK 8
#define DAP_SWDIO 9
#else
#define DAP_SWCLK 12
#define DAP_SWDIO 13
#endif
#define DAP_RESET 3

static struct bflb_device_s *dap_gpio;

static inline void dap_pin_write(uint8_t pin, int value)
{
    if (value) bflb_gpio_set(dap_gpio, pin);
    else bflb_gpio_reset(dap_gpio, pin);
}

static inline void DAP_CONFIG_SWCLK_TCK_write(int v) { dap_pin_write(DAP_SWCLK, v); }
static inline void DAP_CONFIG_SWDIO_TMS_write(int v) { dap_pin_write(DAP_SWDIO, v); }
static inline void DAP_CONFIG_nRESET_write(int v) { dap_pin_write(DAP_RESET, v); }
static inline void DAP_CONFIG_SWCLK_TCK_set(void) { DAP_CONFIG_SWCLK_TCK_write(1); }
static inline void DAP_CONFIG_SWCLK_TCK_clr(void) { DAP_CONFIG_SWCLK_TCK_write(0); }
static inline int DAP_CONFIG_SWCLK_TCK_read(void) { return bflb_gpio_read(dap_gpio, DAP_SWCLK); }
static inline int DAP_CONFIG_SWDIO_TMS_read(void) { return bflb_gpio_read(dap_gpio, DAP_SWDIO); }
static inline int DAP_CONFIG_nRESET_read(void) { return bflb_gpio_read(dap_gpio, DAP_RESET); }
static inline void DAP_CONFIG_TDI_write(int v) { (void)v; }
static inline void DAP_CONFIG_nTRST_write(int v) { (void)v; }
static inline int DAP_CONFIG_TDI_read(void) { return 0; }
static inline int DAP_CONFIG_TDO_read(void) { return 0; }
static inline int DAP_CONFIG_nTRST_read(void) { return 0; }
static inline void DAP_CONFIG_LED(int i, int state) { (void)i; (void)state; }

static inline void DAP_CONFIG_SWDIO_TMS_in(void)
{
    bflb_gpio_init(dap_gpio, DAP_SWDIO, GPIO_INPUT | GPIO_FLOAT | GPIO_SMT_EN);
}

static inline void DAP_CONFIG_SWDIO_TMS_out(void)
{
    bflb_gpio_init(dap_gpio, DAP_SWDIO, GPIO_INPUT | GPIO_OUTPUT | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_1);
}

static inline void DAP_CONFIG_DISCONNECT(void)
{
    bflb_gpio_init(dap_gpio, DAP_SWCLK, GPIO_INPUT | GPIO_FLOAT | GPIO_SMT_EN);
    DAP_CONFIG_SWDIO_TMS_in();
    /* RESET remains an output: the bridge also owns serial-upload reset. */
    DAP_CONFIG_nRESET_write(1);
}

static inline void DAP_CONFIG_SETUP(void)
{
    dap_gpio = bflb_device_get_by_name("gpio");
    pin_release_keep(DAP_SWCLK);
    pin_release_keep(DAP_SWDIO);
    DAP_CONFIG_DISCONNECT();
}

static inline void DAP_CONFIG_CONNECT_SWD(void)
{
    bflb_gpio_init(dap_gpio, DAP_SWCLK, GPIO_INPUT | GPIO_OUTPUT | GPIO_FLOAT | GPIO_SMT_EN | GPIO_DRV_1);
    DAP_CONFIG_SWCLK_TCK_set();
    DAP_CONFIG_SWDIO_TMS_out();
    DAP_CONFIG_SWDIO_TMS_write(1);
    DAP_CONFIG_nRESET_write(1);
}

static inline void bridge_dap_target_reset(void)
{
    DAP_CONFIG_nRESET_write(0);
    bflb_mtimer_delay_ms(1);
    DAP_CONFIG_nRESET_write(1);
}
#define DAP_CONFIG_RESET_TARGET_FN bridge_dap_target_reset

/* One delay unit is 1 us at this platform's fixed 320 MHz CPU clock.
 * GPIO access and interrupts can lengthen the requested SWCLK period.
 */
static inline void DAP_CONFIG_DELAY(uint32_t units)
{
    uint32_t start, now;
    __asm__ volatile ("csrr %0, mcycle" : "=r"(start));
    do {
        __asm__ volatile ("csrr %0, mcycle" : "=r"(now));
    } while ((uint32_t)(now - start) < units * 320u);
}
