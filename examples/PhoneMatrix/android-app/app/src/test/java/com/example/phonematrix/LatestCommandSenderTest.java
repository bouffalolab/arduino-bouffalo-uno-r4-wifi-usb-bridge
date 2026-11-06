package com.example.phonematrix;

import org.junit.Test;
import static org.junit.Assert.*;
import java.util.ArrayList;
import java.util.List;

public class LatestCommandSenderTest {
    private static class Clock implements LatestCommandSender.Scheduler {
        long time, due;
        Runnable task;
        public long now(){return time;}
        public void postDelayed(Runnable r,long delay){task=r;due=time+delay;}
        public void remove(Runnable r){if(task==r)task=null;}
        void advance(long millis){time+=millis;if(task!=null&&time>=due){Runnable r=task;task=null;r.run();}}
    }
    private static class Fixture implements LatestCommandSender.Listener {
        final Clock clock=new Clock();
        final List<byte[]> writes=new ArrayList<>();
        final List<Integer> revisions=new ArrayList<>();
        boolean accept=true;
        int rejected;
        final LatestCommandSender sender=new LatestCommandSender(clock,value->{writes.add(value);return accept;},this,50);
        Fixture(){sender.setReady(true);}
        public void onStarted(LatestCommandSender.Command command){revisions.add(command.revision);}
        public void onRejected(LatestCommandSender.Command command){rejected++;}
        void offer(int value,int revision){assertTrue(sender.offer(new byte[]{(byte)value},revision,"TX "));}
    }
    @Test public void sendsFirstEditImmediatelyAndCoalescesToFinalFrame(){
        Fixture f=new Fixture();f.offer(1,1);
        assertEquals(1,f.writes.size());
        for(int i=2;i<=96;i++)f.offer(i,i);
        assertEquals(1,f.writes.size());
        f.clock.advance(80);f.sender.complete(true);
        assertEquals(2,f.writes.size());assertArrayEquals(new byte[]{96},f.writes.get(1));
        assertEquals(96,f.sender.current().revision);
        f.sender.complete(true);assertNull(f.sender.current());
    }
    @Test public void enforcesIntervalWithoutStarvingContinuousDrawing(){
        Fixture f=new Fixture();f.offer(1,1);f.offer(2,2);f.sender.complete(true);
        f.clock.advance(20);f.offer(3,3);f.clock.advance(29);f.offer(4,4);
        assertEquals(1,f.writes.size());f.clock.advance(1);
        assertEquals(2,f.writes.size());assertArrayEquals(new byte[]{4},f.writes.get(1));
    }
    @Test public void disconnectCancelsBothActiveAndDelayedCommands(){
        Fixture f=new Fixture();f.offer(1,1);f.offer(2,2);f.sender.complete(true);
        f.sender.setReady(false);f.clock.advance(1000);assertEquals(1,f.writes.size());
        assertFalse(f.sender.offer(new byte[]{3},3,"TX "));
        f.sender.setReady(true);f.clock.advance(1000);assertEquals(1,f.writes.size());
        f.offer(4,4);assertEquals(2,f.writes.size());
    }
    @Test public void failureDropsWaitingFramesWithoutAutomaticRetry(){
        Fixture f=new Fixture();f.offer(1,1);f.offer(2,2);f.sender.complete(false);
        f.clock.advance(5000);assertEquals(1,f.writes.size());assertNull(f.sender.current());
        f.offer(3,3);assertEquals(2,f.writes.size());
    }
    @Test public void laterPresetReplacesAnUnsentDrawing(){
        Fixture f=new Fixture();f.offer(1,1);f.offer(2,2);f.offer(3,3);
        f.clock.advance(50);f.sender.complete(true);
        assertArrayEquals(new byte[]{3},f.writes.get(1));
        assertEquals(Integer.valueOf(3),f.revisions.get(1));
    }
    @Test public void storesSnapshotAndHandlesTransportRejection(){
        Fixture f=new Fixture();f.offer(1,1);
        byte[] frame={2};f.sender.offer(frame,2,"TX ");frame[0]=99;
        f.clock.advance(50);f.accept=false;f.sender.complete(true);
        assertArrayEquals(new byte[]{2},f.writes.get(1));
        assertNull(f.sender.current());assertEquals(1,f.rejected);
        f.clock.advance(5000);assertEquals(2,f.writes.size());
    }
}
