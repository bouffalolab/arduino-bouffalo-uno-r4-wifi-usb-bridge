package com.example.phonematrix;

import android.app.Instrumentation;
import android.graphics.RectF;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.widget.TextView;
import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.Test;
import static org.junit.Assert.*;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.List;

/** Real Activity/touch integration, with a fake transport. This is not hardware BLE acceptance. */
public class LiveDrawingTest {
    private static Object get(Object target,String name) {
        try{Field f=target.getClass().getDeclaredField(name);f.setAccessible(true);return f.get(target);}
        catch(Exception e){throw new AssertionError(e);}
    }
    private static void set(Object target,String name,Object value) {
        try{Field f=target.getClass().getDeclaredField(name);f.setAccessible(true);f.set(target,value);}
        catch(Exception e){throw new AssertionError(e);}
    }
    private static void touch(MatrixGridView grid,int action,int column) {
        RectF panel=(RectF)get(grid,"panel");float inset=(Float)get(grid,"inset");
        float x=panel.left+inset+(column+.5f)*(panel.width()-2*inset)/12;
        float y=panel.top+inset+.5f*(panel.height()-2*inset)/8;
        long t=SystemClock.uptimeMillis();MotionEvent event=MotionEvent.obtain(t,t,action,x,y,0);
        grid.dispatchTouchEvent(event);event.recycle();
    }
    @Test public void drawingSendsBeforeFingerLiftAndFinalFrameGetsAcked() {
        Instrumentation i=InstrumentationRegistry.getInstrumentation();
        MainActivity a=TestActivityLauncher.launch(i);
        List<byte[]> writes=new ArrayList<>();
        try{
            i.waitForIdleSync();
            i.runOnMainSync(()->{
                LatestCommandSender commands=(LatestCommandSender)get(a,"commands");
                set(commands,"transport",(LatestCommandSender.Transport)value->{writes.add(value.clone());return true;});
                a.onState("Ready");
                MatrixGridView grid=(MatrixGridView)get(a,"grid");
                touch(grid,MotionEvent.ACTION_DOWN,0);
                assertEquals(1,writes.size());assertEquals(13,writes.get(0).length);
                assertEquals(0x10,writes.get(0)[0]);assertEquals(0x80,writes.get(0)[1]&255);
                touch(grid,MotionEvent.ACTION_MOVE,1);touch(grid,MotionEvent.ACTION_MOVE,2);
                assertEquals(1,writes.size());
                a.onCommand(writes.get(0),"OK:RAW",true);
                assertNotEquals("Sent",((TextView)get(a,"canvasStatus")).getText().toString());
            });
            SystemClock.sleep(100);i.waitForIdleSync();
            i.runOnMainSync(()->{
                assertEquals(2,writes.size());assertEquals(0xe0,writes.get(1)[1]&255);
                a.onCommand(writes.get(1),"OK:RAW",true);
                assertEquals("Sent",((TextView)get(a,"canvasStatus")).getText().toString());
                MatrixGridView grid=(MatrixGridView)get(a,"grid");touch(grid,MotionEvent.ACTION_UP,2);
                a.onState("Disconnected");
                touch(grid,MotionEvent.ACTION_DOWN,4);touch(grid,MotionEvent.ACTION_UP,4);
                assertEquals(2,writes.size());assertEquals("Draft",((TextView)get(a,"canvasStatus")).getText().toString());
                a.onState("Ready");
            });
            SystemClock.sleep(100);i.waitForIdleSync();
            i.runOnMainSync(()->assertEquals(2,writes.size()));
        }finally{i.runOnMainSync(a::finish);}
    }
}
