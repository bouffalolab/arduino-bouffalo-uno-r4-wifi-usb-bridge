package com.example.phonematrix;

import android.app.Instrumentation;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothManager;
import android.content.Context;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.ListAdapter;
import android.widget.Spinner;
import android.widget.TextView;
import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.Test;
import static org.junit.Assert.*;
import java.lang.reflect.Field;
import java.util.List;
import java.util.ArrayList;
import java.util.Collections;
import java.util.concurrent.atomic.AtomicReference;

/** Exercises Android Spinner layout callbacks using injected scan results, without BLE connections. */
public class ScanSelectionTest {
    private static Object get(Object target,String name){
        try{Field f=target.getClass().getDeclaredField(name);f.setAccessible(true);return f.get(target);}
        catch(Exception e){throw new AssertionError(e);}
    }
    private static void main(Instrumentation i,Runnable action){
        AtomicReference<Throwable> failure=new AtomicReference<>();
        i.runOnMainSync(()->{try{action.run();}catch(Throwable t){failure.set(t);}});
        if(failure.get()!=null)throw new AssertionError(failure.get());
        i.waitForIdleSync();
    }
    private static void visibleText(View view,List<String> labels){
        if(view.getVisibility()!=View.VISIBLE)return;
        if(view instanceof TextView){
            String text=((TextView)view).getText().toString();
            if(!text.isEmpty()&&!text.equals("⌄"))labels.add(text);
        }
        if(view instanceof ViewGroup){
            ViewGroup group=(ViewGroup)view;
            for(int n=0;n<group.getChildCount();n++)visibleText(group.getChildAt(n),labels);
        }
    }
    private static void singleLabel(Spinner picker,String expected){
        List<String> labels=new ArrayList<>();visibleText((View)picker.getParent(),labels);
        assertEquals("The field must render exactly one device/status label",Collections.singletonList(expected),labels);
    }
    @Test public void otherDevicesNeverBecomeTheDefaultBeforeLateMatrix(){
        Instrumentation i=InstrumentationRegistry.getInstrumentation();
        MainActivity a=TestActivityLauncher.launch(i);
        BluetoothManager manager=(BluetoothManager)i.getTargetContext().getSystemService(Context.BLUETOOTH_SERVICE);
        BluetoothDevice other=manager.getAdapter().getRemoteDevice("02:00:00:00:00:01");
        BluetoothDevice target=manager.getAdapter().getRemoteDevice("02:00:00:00:00:02");
        try{
            i.waitForIdleSync();
            Spinner picker=(Spinner)get(a,"picker");Button connect=(Button)get(a,"connectButton");
            main(i,()->{singleLabel(picker,"No devices");a.onState("Scanning (10 s)");});
            main(i,()->{singleLabel(picker,"Select device");a.onState("Scan stopped");});
            main(i,()->{singleLabel(picker,"No devices found");assertFalse(connect.isEnabled());a.onState("Scanning (10 s)");a.onFound(other,"Other device");});
            main(i,()->{
                assertEquals("Select device",picker.getSelectedItem().toString());
                singleLabel(picker,"Select device");
                assertFalse(connect.isEnabled());assertFalse(((ListAdapter)picker.getAdapter()).isEnabled(0));
                assertTrue(picker.getAdapter().getItem(1).toString().startsWith("Other device"));
                a.onState("Scan stopped");
            });
            main(i,()->{
                assertEquals("Select device",picker.getSelectedItem().toString());assertFalse(connect.isEnabled());
                picker.setSelection(1);
            });
            main(i,()->{
                assertTrue(connect.isEnabled());
                assertEquals(other.getAddress(),((List<?>)get(a,"deviceKeys")).get(picker.getSelectedItemPosition()));
                a.onFound(target,"BL616 Matrix");
            });
            main(i,()->{
                assertEquals(0,picker.getSelectedItemPosition());assertTrue(connect.isEnabled());
                assertTrue(picker.getAdapter().getItem(0).toString().startsWith("BL616 Matrix"));
                singleLabel(picker,picker.getSelectedItem().toString());
                assertEquals(target.getAddress(),((List<?>)get(a,"deviceKeys")).get(0));
                picker.setSelection(1);
            });
            main(i,()->{a.onFound(target,"BL616 Matrix");a.onFound(other,"Other device");});
            main(i,()->{
                assertTrue(picker.getSelectedItem().toString().startsWith("Other device"));
                singleLabel(picker,picker.getSelectedItem().toString());
                assertEquals(other.getAddress(),((List<?>)get(a,"deviceKeys")).get(picker.getSelectedItemPosition()));
            });
        }finally{i.runOnMainSync(a::finish);}
    }
}
