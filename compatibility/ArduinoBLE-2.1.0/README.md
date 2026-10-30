# ArduinoBLE 2.1.0 public-address bonded reconnection patch

This patch targets the RA4M1 Host library. It does not change the BL616CL Core, Controller, or bridge protocol. Ordinary unencrypted GATT, official BLE examples, and the existing Bumble public-address/identity-key distribution path can still use the original library. For bonded reconnection with a BlueZ public-address peer that does not send an initiator IdKey, apply this patch and rebuild the RA program.

## Problem and correction

Official 2.1.0 always replies with IdKey(2) for both key distribution fields without intersecting the request masks. It invokes the long-term key storage callback only upon receiving an Identity Address. On hardware, BlueZ requested initiator=13 (without IdKey), responder=15. Initial encryption succeeded, but BlueZ did not send its Identity Address, so the RA side did not store the LTK. On reconnection, getLTK returned 0 and the Controller reported encryption status6. The original failure is reproducible; its raw records are retained.

The patch intersects both key distribution masks with the IdKey supported by this library. If a public-address peer does not distribute IdKey in this Secure Connections pairing, the original storeLTK callback is invoked with its known public address when encryption is established. Peers distributing IdKey retain the original identity-address callback path. The application remains responsible for storing and retrieving keys correctly; no fixed success result is supplied.

This patch covers only the public-address scenario tested on hardware. RPA, random-address peers without IdKey, and key persistence across power loss are outside its acceptance scope. Test keys remain in RAM; LTK/IRK and numeric comparison codes are not logged.

## Standard Arduino installation and build

Install Renesas UNO core 1.6.0 and ArduinoBLE 2.1.0. Locate the user sketchbook libraries directory through the Arduino IDE or CLI configuration and set BLE_LIBRARY to its ArduinoBLE directory. Do not modify the BL platform installation or apply the patch automatically during ordinary sketch builds.

First verify HCI.cpp and L2CAPSignaling.cpp against the original SHA256 values in manifest.json, then run from this repository root:

```sh
patch --dry-run -p1 -d "$BLE_LIBRARY" < compatibility/ArduinoBLE-2.1.0/public-peer-bond.patch
patch -p1 -d "$BLE_LIBRARY" < compatibility/ArduinoBLE-2.1.0/public-peer-bond.patch
arduino-cli compile -b arduino:renesas_uno:unor4wifi --build-path "$RA_BUILD" "$RA_SKETCH"
arduino-cli upload -b arduino:renesas_uno:unor4wifi -p "$RA_PORT" --input-dir "$RA_BUILD" "$RA_SKETCH"
```

After application, both source file hashes must match patched_sha256 in manifest.json. Do not apply the patch twice. If the version or hashes differ, stop and reinstall the exact version rather than masking the difference with fuzzy edits. Arduino Library Manager updates or reinstalls replace the user library, requiring verification again. Compatibility with future versions is not promised.

To revert, use patch -R in the same library or reinstall official 2.1.0, then rebuild the RA program. The original BlueZ bonded reconnection limitation returns after rollback. The BL firmware does not need reflashing for this change.

## Hardware evidence and provenance

ARDUINO-53: Linux x86_64, RA4M1/Renesas1.6.0, BL616CL 1V0 candidate 9d345c2d, and real AX200/BlueZ5.72 and Bumble0.0.236. The original BlueZ path failed. With the patch, pairing rejection/acceptance, 10 encrypted read/write/notification rounds, and reconnection with new pairing disabled passed. Bumble identity-key distribution, encrypted reconnection, and pairing-window expiry regressions passed. Actual repeated reconnection counts and raw logs are listed in the corresponding task supplement acceptance package.

Source files were verified byte-for-byte against the official 2.1.0 tag before modification. Original and patched SHA values are in manifest.json. The original LGPL-2.1 license is retained as LICENSE.ArduinoBLE. This patch changes SMP negotiation and LTK storage timing without changing third-party copyright notices. It has not been submitted to Arduino upstream and is not presented as an official fix.
