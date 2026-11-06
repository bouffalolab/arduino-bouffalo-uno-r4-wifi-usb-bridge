package com.example.phonematrix;

import android.app.Instrumentation;
import android.graphics.Rect;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ScrollView;
import android.widget.TextView;
import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.Test;
import java.lang.reflect.Field;
import java.util.concurrent.atomic.AtomicReference;
import static org.junit.Assert.*;

/** XML inflation, visible geometry and dialog binding on the connected phone. No BLE transport. */
public class XmlLayoutTest {
    private static void main(Instrumentation i, Runnable action) {
        AtomicReference<Throwable> failure = new AtomicReference<>();
        i.runOnMainSync(() -> { try { action.run(); } catch (Throwable t) { failure.set(t); } });
        if (failure.get() != null) throw new AssertionError(failure.get());
        i.waitForIdleSync();
    }

    private static TextView log(MainActivity a) {
        try {
            Field f = MainActivity.class.getDeclaredField("log");
            f.setAccessible(true);
            return (TextView) f.get(a);
        } catch (Exception e) { throw new AssertionError(e); }
    }

    private static void noScroll(View v) {
        assertFalse("Main screen must not scroll", v instanceof ScrollView);
        if (v instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) v;
            for (int n = 0; n < group.getChildCount(); n++) noScroll(group.getChildAt(n));
        }
    }

    private static void fits(MainActivity a) {
        View page = a.findViewById(R.id.page);
        noScroll(page);
        int[] ids = {R.id.matrix_grid, R.id.log_button, R.id.help_button, R.id.device_picker,
                R.id.scan_button, R.id.connect_button, R.id.disconnect_button, R.id.demo_button,
                R.id.heart_button, R.id.smile_button, R.id.checker_button, R.id.clear_button,
                R.id.digit_0, R.id.digit_1, R.id.digit_2, R.id.digit_3, R.id.digit_4,
                R.id.digit_5, R.id.digit_6, R.id.digit_7, R.id.digit_8, R.id.digit_9};
        float density = a.getResources().getDisplayMetrics().density;
        for (int id : ids) {
            View v = a.findViewById(id);
            if (v.getVisibility() == View.GONE) continue;
            Rect visible = new Rect();
            assertTrue("Control must be visible: " + id, v.getGlobalVisibleRect(visible));
            assertEquals("No vertical clipping: " + id, v.getHeight(), visible.height());
            assertEquals("No horizontal clipping: " + id, v.getWidth(), visible.width());
            assertTrue("Touch height >= 48dp: " + id, v.getHeight() >= Math.round(48 * density));
            assertTrue("Touch width >= 48dp: " + id, v.getWidth() >= Math.round(48 * density));
        }
    }

    @Test public void screenFitsOfflineConnectedAndErrorStates() {
        Instrumentation i = InstrumentationRegistry.getInstrumentation();
        MainActivity a = TestActivityLauncher.launch(i);
        try {
            i.waitForIdleSync();
            main(i, () -> { fits(a); a.onState("Ready"); });
            main(i, () -> {
                fits(a);
                assertTrue(a.findViewById(R.id.connection_badge).isActivated());
                assertEquals(View.GONE, a.findViewById(R.id.connect_button).getVisibility());
                a.onError("Test error: this verifies the message row fits on screen.");
            });
            main(i, () -> {
                fits(a);
                assertTrue(a.findViewById(R.id.connection_badge).isSelected());
                assertTrue(a.findViewById(R.id.canvas_status).isSelected());
                a.onState("Disconnected");
            });
            main(i, () -> { fits(a); assertFalse(a.findViewById(R.id.canvas_status).isSelected()); });
        } finally { i.runOnMainSync(a::finish); }
    }

    @Test public void logReopensWithHistoryAndUpdatesWhileOpen() {
        Instrumentation i = InstrumentationRegistry.getInstrumentation();
        MainActivity a = TestActivityLauncher.launch(i);
        try {
            i.waitForIdleSync();
            main(i, () -> {
                a.onState("Scan stopped");
                a.findViewById(R.id.log_button).performClick();
                assertTrue(log(a).getText().toString().contains("Scan stopped"));
                a.onStatus("OK:RAW", false);
                assertTrue(log(a).getText().toString().contains("RX OK:RAW"));
            });
            i.sendKeyDownUpSync(KeyEvent.KEYCODE_BACK);
            i.waitForIdleSync();
            main(i, () -> {
                assertNull(log(a));
                a.findViewById(R.id.log_button).performClick();
                assertTrue(log(a).getText().toString().contains("RX OK:RAW"));
            });
            i.sendKeyDownUpSync(KeyEvent.KEYCODE_BACK);
            i.waitForIdleSync();
        } finally { i.runOnMainSync(a::finish); }
    }
}
