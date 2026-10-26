# Third-party notices

## Free-DAP

`UNOR4USBBridgeBL/freedap.c` and `freedap.h` come from
`arduino/uno-r4-wifi-usb-bridge` commit
`94d5bb2e8c2cb5492345bfb84d787fde65d1c183` (official 0.6.0 baseline).
The BL616CL `dap_config.h` replaces the ESP32 GPIO/timing port.
The command engine has one local change: reject zero/negative SWJ clock
values before division. The original copyright and BSD-3-Clause notices
are retained. JTAG and SWO are not enabled.

Source archive:
`https://codeload.github.com/arduino/uno-r4-wifi-usb-bridge/tar.gz/94d5bb2`

Original SHA-256:

- archive: `aa99d4ff29f252d7f01136b143742f284673e6caafe0bf8dc40957ba30ea8069`
- freedap.c: `6a94e3b79f588938242c67fa554e419beb131db70b85d30a0c1867aa38409be4`
- freedap.h: `3c35445e249a0f1cffb7e38b671c76311363c255c3dbd8952573a9dcd6535460`

Retain this notice when distributing a binary containing Free-DAP.
This notice covers the new DAP source; dependency redistribution remains
subject to the project's separate release review.

```text
/*
 * Copyright (c) 2016-2021, Alex Taradov <alex@taradov.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
```
