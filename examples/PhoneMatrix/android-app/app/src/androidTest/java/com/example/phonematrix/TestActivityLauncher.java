package com.example.phonematrix;

import android.app.Instrumentation;
import android.os.ParcelFileDescriptor;
import java.io.IOException;

/** Launch through ADB's shell identity because MIUI blocks background starts from the test UID. */
final class TestActivityLauncher {
    static MainActivity launch(Instrumentation instrumentation) {
        Instrumentation.ActivityMonitor monitor = instrumentation.addMonitor(MainActivity.class.getName(), null, false);
        try {
            String command = "am start -W -f 0x14000000 -n com.example.phonematrix/.MainActivity";
            try (ParcelFileDescriptor.AutoCloseInputStream output = new ParcelFileDescriptor.AutoCloseInputStream(
                    instrumentation.getUiAutomation().executeShellCommand(command))) {
                byte[] buffer = new byte[1024];
                while (output.read(buffer) != -1) { /* Drain the launch result before checking the monitor. */ }
            }
            MainActivity activity = (MainActivity) instrumentation.waitForMonitorWithTimeout(monitor, 10000);
            if (activity == null) throw new AssertionError("MainActivity did not launch");
            instrumentation.waitForIdleSync();
            return activity;
        } catch (IOException e) { throw new AssertionError(e); }
        finally { instrumentation.removeMonitor(monitor); }
    }
}
