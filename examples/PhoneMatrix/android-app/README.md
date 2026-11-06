# PhoneMatrix

Control the 12×8 LED matrix on a BL-based UNO R4 WiFi from an Android phone over BLE. Draw live, display preset patterns and digits, or play a preset loop.

## Requirements

- An Android 6.0 or newer phone with BLE support and Bluetooth enabled.
- A BL-based UNO R4 WiFi running the BLE HCI over AT bridge firmware.
- The board's RA4M1 controller must already run the PhoneMatrix BLE sketch and advertise as **BL616 Matrix**. Obtain the board firmware separately from its maintainer. The sketch source is [PhoneMatrix.ino](../PhoneMatrix.ino) in the parent folder.

## Install the app

1. Open the repository root in Android Studio and wait for Gradle sync to finish.
2. Enable USB debugging on your phone, connect it by USB, and accept the debugging authorization.
3. Select the phone in Android Studio and click **Run** to install and launch the app.

## Connect the board

1. Power the board, open the app, and allow **Nearby devices** access. On Android 11 and earlier, allow location access and enable the system location setting.
2. Tap **Scan** and wait for results.
3. When **BL616 Matrix** appears, the app moves it to the top and selects it automatically. Tap **Connect**.
4. Wait for **Ready**, then use the matrix controls. Tap **Disconnect** to end the connection.

If the board does not appear, check that it is running and is not connected to another phone, then tap **Scan** again.

## Use the matrix

| Action | Result |
| --- | --- |
| Tap or drag on the canvas | Turn pixels on or off; changes send live while connected |
| Tap the heart, smile, or grid icon | Display the selected preset immediately |
| Tap the trash icon | Clear the matrix |
| Tap a digit from 0–9 | Display that digit immediately |
| Tap the play icon | Loop through heart, smile, checker, and clear |
| Tap the playback button again (now a stop icon) | Stop the loop |

Drawing, choosing a preset, or tapping a digit also stops playback. The phone icons are colored; the board's LED matrix is monochrome.

While disconnected, canvas edits stay on the phone. After reconnecting, continue drawing to send the current frame.

## Status and help

- **Draft**: a local draft.
- **Sending…**: waiting for the board to acknowledge the command.
- **Sent**: the current frame has been acknowledged by the board.
- **Failed / Attention**: an error occurred. Open the activity log at the top to check the cause and reconnect if needed.

Tap **ⓘ** at the top right for help. Long-press an icon button to see its action name.
