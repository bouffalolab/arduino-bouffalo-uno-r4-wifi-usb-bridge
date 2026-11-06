package com.example.phonematrix;

/** Main-thread dispatcher: one command awaiting ACK, plus only the latest unsent frame. */
public final class LatestCommandSender {
    public interface Scheduler {
        long now();
        void postDelayed(Runnable task, long delayMillis);
        void remove(Runnable task);
    }
    public interface Transport { boolean send(byte[] value); }
    public interface Listener {
        void onStarted(Command command);
        void onRejected(Command command);
    }
    public static final class Command {
        public final byte[] value;
        public final int revision;
        public final String prefix;
        private Command(byte[] value, int revision, String prefix) {
            this.value=value.clone();this.revision=revision;this.prefix=prefix;
        }
    }
    private final Scheduler scheduler;
    private final Transport transport;
    private final Listener listener;
    private final long intervalMillis;
    private final Runnable flush=this::flush;
    private Command active, latest;
    private boolean ready, hasStarted;
    private long lastStart;

    public LatestCommandSender(Scheduler scheduler, Transport transport, Listener listener, long intervalMillis) {
        this.scheduler=scheduler;this.transport=transport;this.listener=listener;this.intervalMillis=intervalMillis;
    }
    public void setReady(boolean value) {
        ready=value;
        if(!value){scheduler.remove(flush);active=null;latest=null;hasStarted=false;}
    }
    public boolean offer(byte[] value, int revision, String prefix) {
        if(!ready)return false;
        latest=new Command(value,revision,prefix);
        flush();
        return true;
    }
    public Command current(){return active;}
    public void complete(boolean success) {
        if(active==null)return;
        active=null;
        if(!success){latest=null;scheduler.remove(flush);return;}
        flush();
    }
    private void flush() {
        scheduler.remove(flush);
        if(!ready||active!=null||latest==null)return;
        long delay=hasStarted?intervalMillis-(scheduler.now()-lastStart):0;
        if(delay>0){scheduler.postDelayed(flush,delay);return;}
        Command command=latest;latest=null;active=command;
        hasStarted=true;lastStart=scheduler.now();
        listener.onStarted(command);
        if(!transport.send(command.value)&&active==command){
            active=null;latest=null;listener.onRejected(command);
        }
    }
}
