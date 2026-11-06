package com.example.phonematrix;

import android.annotation.SuppressLint;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanResult;
import android.content.Context;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import java.nio.charset.StandardCharsets;

/** Serializes Android GATT operations and pairs each write with its application ACK. */
public final class MatrixGattSession {
    public interface Listener {
        /** Scan results contain a non-empty, trimmed device name. */
        void onFound(BluetoothDevice device,String name);
        void onState(String state);
        void onStatus(String value, boolean fromRead);
        void onCommand(byte[] value, String result, boolean success);
        void onError(String message);
    }
    private final Context context; private final Listener listener; private final Handler handler = new Handler(Looper.getMainLooper());
    private BluetoothLeScanner scanner; private BluetoothGatt gatt; private BluetoothGattCharacteristic command, status;
    private boolean ready, writeOk, ackOk; private byte[] pending; private String expected; private final Runnable timeout;

    public MatrixGattSession(Context c, Listener l) { context=c.getApplicationContext(); listener=l; timeout = () -> { if (pending != null) { byte[] p=pending; pending=null; listener.onCommand(p,"TIMEOUT",false); } }; }
    @SuppressLint("MissingPermission") public void scan() {
        BluetoothManager bm=(BluetoothManager)context.getSystemService(Context.BLUETOOTH_SERVICE); BluetoothAdapter a=bm.getAdapter();
        if(a==null||!a.isEnabled()){listener.onError("Bluetooth is disabled");return;} scanner=a.getBluetoothLeScanner();
        if(scanner==null){listener.onError("BLE scanner unavailable");return;} scanner.startScan(scanCallback); listener.onState("Scanning (10 s)");
        handler.postDelayed(this::stopScan,10000);
    }
    @SuppressLint("MissingPermission") public void stopScan(){if(scanner!=null){scanner.stopScan(scanCallback);scanner=null;listener.onState("Scan stopped");}}
    private final ScanCallback scanCallback=new ScanCallback(){@Override public void onScanResult(int type,ScanResult r){
        // A later scan response can supply the name even when the first packet has none.
        String name=r.getScanRecord()==null?null:r.getScanRecord().getDeviceName();
        if(name==null||name.trim().isEmpty()){
            try{name=r.getDevice().getName();}catch(SecurityException e){return;}
        }
        if(name!=null&&!name.trim().isEmpty())listener.onFound(r.getDevice(),name.trim());
    }};
    @SuppressLint("MissingPermission") public void connect(BluetoothDevice d) { stopScan(); close(); listener.onState("Connecting "+safeName(d)); gatt=d.connectGatt(context,false,gattCallback,BluetoothDevice.TRANSPORT_LE); }
    @SuppressLint("MissingPermission") public void close(){ready=false;pending=null;handler.removeCallbacks(timeout);if(gatt!=null){gatt.disconnect();gatt.close();gatt=null;}listener.onState("Disconnected");}
    private String safeName(BluetoothDevice d){try{String name=d.getName();return name!=null?name:d.getAddress();}catch(SecurityException e){return "device";}}
    @SuppressLint("MissingPermission") public boolean send(byte[] value){if(!ready||pending!=null)return false;try{expected=MatrixProtocol.expectedStatus(value);}catch(IllegalArgumentException e){return false;}byte[] frame=value.clone();pending=frame;writeOk=false;ackOk=false;command.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);boolean started;
        if(Build.VERSION.SDK_INT>=33) started=gatt.writeCharacteristic(command,frame,BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)==0; else {command.setValue(frame);started=gatt.writeCharacteristic(command);} if(!started){pending=null;listener.onError("GATT write could not start");return false;} handler.postDelayed(timeout,3000); return true; }
    private void finishIfReady(){if(pending!=null&&writeOk&&ackOk){byte[] p=pending;pending=null;handler.removeCallbacks(timeout);listener.onCommand(p,expected,true);}}
    private final BluetoothGattCallback gattCallback=new BluetoothGattCallback(){
        @Override public void onConnectionStateChange(BluetoothGatt g,int st,int newState){if(newState==BluetoothGatt.STATE_CONNECTED){listener.onState("Connected; discovering services");g.discoverServices();}else{ready=false;handler.removeCallbacks(timeout);if(pending!=null){byte[] p=pending;pending=null;listener.onCommand(p,"DISCONNECTED",false);}listener.onState("Disconnected ("+st+")");}}
        @Override public void onServicesDiscovered(BluetoothGatt g,int st){if(st!=BluetoothGatt.GATT_SUCCESS){listener.onError("Service discovery failed: "+st);return;} BluetoothGattService s=g.getService(MatrixProtocol.SERVICE); if(s==null){listener.onError("Matrix service not found");return;} command=s.getCharacteristic(MatrixProtocol.COMMAND);status=s.getCharacteristic(MatrixProtocol.STATUS); int sp=status==null?0:status.getProperties(); if(command==null||status==null||(command.getProperties()&BluetoothGattCharacteristic.PROPERTY_WRITE)==0||(sp&(BluetoothGattCharacteristic.PROPERTY_READ|BluetoothGattCharacteristic.PROPERTY_NOTIFY))!=(BluetoothGattCharacteristic.PROPERTY_READ|BluetoothGattCharacteristic.PROPERTY_NOTIFY)){listener.onError("Firmware/service mismatch");return;} if(!g.setCharacteristicNotification(status,true)){listener.onError("Local notification registration failed");return;} BluetoothGattDescriptor c=status.getDescriptor(MatrixProtocol.CCCD);if(c==null){listener.onError("Status CCCD not found");return;} boolean started; if(Build.VERSION.SDK_INT>=33)started=g.writeDescriptor(c,BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE.clone())==0;else{c.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);started=g.writeDescriptor(c);} if(!started){listener.onError("CCCD write could not start");return;} listener.onState("Subscribing to status");}
        @Override public void onDescriptorWrite(BluetoothGatt g,BluetoothGattDescriptor d,int st){if(d.getUuid().equals(MatrixProtocol.CCCD)&&st==BluetoothGatt.GATT_SUCCESS){g.readCharacteristic(status);listener.onState("Reading initial status");}else if(st!=BluetoothGatt.GATT_SUCCESS)listener.onError("CCCD write failed: "+st);}
        @Override public void onCharacteristicRead(BluetoothGatt g,BluetoothGattCharacteristic c,byte[] v,int st){if(c.getUuid().equals(MatrixProtocol.STATUS)){if(st==BluetoothGatt.GATT_SUCCESS){listener.onStatus(new String(v,StandardCharsets.US_ASCII),true);ready=true;listener.onState("Ready");}else listener.onError("Status read failed: "+st);}}
        @Override public void onCharacteristicRead(BluetoothGatt g,BluetoothGattCharacteristic c,int st){if(Build.VERSION.SDK_INT<33&&c.getUuid().equals(MatrixProtocol.STATUS)&&st==BluetoothGatt.GATT_SUCCESS){listener.onStatus(new String(c.getValue(),StandardCharsets.US_ASCII),true);ready=true;listener.onState("Ready");}}
        @Override public void onCharacteristicWrite(BluetoothGatt g,BluetoothGattCharacteristic c,int st){if(c.getUuid().equals(MatrixProtocol.COMMAND)){writeOk=st==BluetoothGatt.GATT_SUCCESS;if(!writeOk)listener.onError("Command write failed: "+st);finishIfReady();}}
        @Override public void onCharacteristicChanged(BluetoothGatt g,BluetoothGattCharacteristic c,byte[] v){if(c.getUuid().equals(MatrixProtocol.STATUS))statusValue(new String(v,StandardCharsets.US_ASCII));}
        @Override public void onCharacteristicChanged(BluetoothGatt g,BluetoothGattCharacteristic c){if(Build.VERSION.SDK_INT<33&&c.getUuid().equals(MatrixProtocol.STATUS))statusValue(new String(c.getValue(),StandardCharsets.US_ASCII));}
        private void statusValue(String value){listener.onStatus(value,false);if(pending!=null&&value.equals(expected)){ackOk=true;finishIfReady();}}
    };
}
