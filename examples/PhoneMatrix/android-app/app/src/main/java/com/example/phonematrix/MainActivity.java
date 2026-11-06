package com.example.phonematrix;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.bluetooth.BluetoothDevice;
import android.content.pm.PackageManager;
import android.graphics.Insets;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;

/** Binds the XML screen and connects user interactions to the BLE session and sender. */
public final class MainActivity extends Activity implements MatrixGattSession.Listener {
    private static final int REQUEST_BLE = 41;
    private static final String PREFERRED_DEVICE_NAME = "BL616 Matrix";
    private final Map<String, BluetoothDevice> devices=new LinkedHashMap<>();
    private final Map<String, String> deviceNames=new LinkedHashMap<>();
    private final ArrayList<String> deviceLabels=new ArrayList<>();
    // A null key is a disabled placeholder, never a connectable device.
    private final ArrayList<String> deviceKeys=new ArrayList<>();
    private final Handler handler=new Handler(Looper.getMainLooper());
    private final Runnable demoStep=this::runDemoStep;
    private ArrayAdapter<String> deviceAdapter;
    private MatrixGattSession session;
    private LatestCommandSender commands;
    private MatrixGridView grid;
    private Spinner picker;
    private TextView state, connectionBadge, log, canvasStatus;
    private final StringBuilder activityLog = new StringBuilder();
    private Button scanButton, connectButton, disconnectButton, demoButton;
    private boolean demo, ready, linked, scanning, connecting;
    private int demoIndex, draftRevision;

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        bindUi();
        session=new MatrixGattSession(this,this);
        commands=new LatestCommandSender(new LatestCommandSender.Scheduler(){
            public long now(){return SystemClock.uptimeMillis();}
            public void postDelayed(Runnable task,long delay){handler.postDelayed(task,delay);}
            public void remove(Runnable task){handler.removeCallbacks(task);}
        },session::send,new LatestCommandSender.Listener(){
            public void onStarted(LatestCommandSender.Command command){
                setCanvasStatus(R.string.sending,true,false);log(command.prefix+MatrixProtocol.hex(command.value));
            }
            public void onRejected(LatestCommandSender.Command command){
                stopDemo();setCanvasStatus(R.string.failed,false,true);log("TX could not start");
            }
        },50);
        if(!hasBlePermission()){showConnectionMessage(getString(R.string.permission_required));requestBlePermission();}
    }

    private void bindUi() {
        setContentView(R.layout.activity_main);
        grid = findViewById(R.id.matrix_grid);
        picker = findViewById(R.id.device_picker);
        state = findViewById(R.id.connection_message);
        connectionBadge = findViewById(R.id.connection_badge);
        canvasStatus = findViewById(R.id.canvas_status);
        scanButton = findViewById(R.id.scan_button);
        connectButton = findViewById(R.id.connect_button);
        disconnectButton = findViewById(R.id.disconnect_button);
        demoButton = findViewById(R.id.demo_button);

        View page = findViewById(R.id.page);
        page.setOnApplyWindowInsetsListener((v, insets) -> {
            if (Build.VERSION.SDK_INT >= 30) {
                Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                v.setPadding(bars.left, bars.top, bars.right, bars.bottom);
            } else {
                v.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(),
                        insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
            }
            return insets;
        });
        int flags = View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR;
        if (Build.VERSION.SDK_INT >= 26) flags |= View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR;
        getWindow().getDecorView().setSystemUiVisibility(flags);
        page.requestApplyInsets();

        resetDiscovery();
        deviceLabels.set(0, getString(R.string.no_devices));
        deviceAdapter = new ArrayAdapter<String>(this, R.layout.item_device, deviceLabels) {
            @Override public boolean areAllItemsEnabled() { return false; }
            @Override public boolean isEnabled(int position) { return deviceKeys.get(position) != null; }
            @Override public View getView(int position, View recycled, ViewGroup parent) {
                View view = super.getView(position, recycled, parent);
                view.setActivated(deviceKeys.get(position) != null);
                return view;
            }
        };
        deviceAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        picker.setAdapter(deviceAdapter);
        picker.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                updateConnectionControls();
            }
            @Override public void onNothingSelected(AdapterView<?> parent) {}
        });
        scanButton.setOnClickListener(v -> {
            if (!hasBlePermission()) { requestBlePermission(); return; }
            resetDiscovery();
            deviceAdapter.notifyDataSetChanged();
            picker.setSelection(0);
            picker.setContentDescription(getString(R.string.device_select_accessibility));
            updateConnectionControls();
            session.scan();
        });
        connectButton.setOnClickListener(v -> {
            BluetoothDevice device = devices.get(selectedDeviceKey());
            if (device != null) { stopDemo(); session.connect(device); }
        });
        disconnectButton.setOnClickListener(v -> { stopDemo(); session.close(); });
        findViewById(R.id.log_button).setOnClickListener(v -> showActivityLog());
        findViewById(R.id.help_button).setOnClickListener(v -> showHelp());
        demoButton.setOnClickListener(v -> {
            if (demo) stopDemo();
            else { demo = true; demoIndex = 0; updateDemoButton(); runDemoStep(); }
        });
        grid.setOnChangeListener(() -> { stopDemo(); markDraft(); if (ready) sendRaw(); });
        bindPreset(R.id.heart_button, 1);
        bindPreset(R.id.smile_button, 2);
        bindPreset(R.id.checker_button, 3);
        bindPreset(R.id.clear_button, 0);
        int[] digitIds = {R.id.digit_0, R.id.digit_1, R.id.digit_2, R.id.digit_3, R.id.digit_4,
                R.id.digit_5, R.id.digit_6, R.id.digit_7, R.id.digit_8, R.id.digit_9};
        for (int n = 0; n < digitIds.length; n++) bindDigit(digitIds[n], n);
        updateConnectionControls();
    }

    private void bindPreset(int id, int opcode) {
        findViewById(id).setOnClickListener(v -> { stopDemo(); sendPreset(opcode); });
    }

    private void bindDigit(int id, int digit) {
        View button = findViewById(id);
        button.setContentDescription(getString(R.string.display_digit, digit));
        button.setOnClickListener(v -> {
            stopDemo();
            grid.setPixels(DigitPatterns.pixels(digit));
            markDraft();
            sendRaw();
        });
    }

    private void showHelp() {
        new AlertDialog.Builder(this).setTitle(R.string.controls).setMessage(R.string.help_message)
                .setPositiveButton(R.string.close, null).show();
    }

    private void showActivityLog() {
        View details = getLayoutInflater().inflate(R.layout.dialog_activity_log, null);
        log = details.findViewById(R.id.activity_log);
        log.setText(activityLog);
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle(R.string.activity_log)
                .setView(details).setPositiveButton(R.string.close, null).create();
        dialog.setOnDismissListener(d -> log = null);
        dialog.show();
    }

    private void showConnectionMessage(String text) {
        state.setText(text);
        state.setVisibility(View.VISIBLE);
    }

    private void setCanvasStatus(int text, boolean active, boolean failed) {
        canvasStatus.setText(text);
        canvasStatus.setActivated(active);
        canvasStatus.setSelected(failed);
    }

    private void enabled(Button button,boolean value){button.setEnabled(value);button.setAlpha(value?1f:.4f);}
    private void resetDiscovery(){
        devices.clear();deviceNames.clear();deviceLabels.clear();deviceKeys.clear();
        deviceLabels.add(getString(R.string.select_device));deviceKeys.add(null);
    }
    private void setDevicePlaceholder(String text){
        if(!deviceKeys.isEmpty()&&deviceKeys.get(0)==null){deviceLabels.set(0,text);deviceAdapter.notifyDataSetChanged();}
    }
    private String selectedDeviceKey(){
        int selected=picker.getSelectedItemPosition();
        return selected>=0&&selected<deviceKeys.size()?deviceKeys.get(selected):null;
    }
    private void updateConnectionControls(){
        enabled(scanButton,!scanning&&!linked&&!connecting);
        enabled(connectButton,selectedDeviceKey()!=null&&!linked&&!connecting);enabled(disconnectButton,linked||connecting);
        connectButton.setVisibility(linked||connecting?View.GONE:View.VISIBLE);disconnectButton.setVisibility(linked||connecting?View.VISIBLE:View.GONE);
        boolean scanPrimary=selectedDeviceKey()==null;
        scanButton.setActivated(scanPrimary);
        connectButton.setActivated(!scanPrimary);
        enabled(demoButton,ready);
    }
    private void updateDemoButton(){((IconButton)demoButton).setAction(demo?"Stop demo":"Start demo");demoButton.setSelected(demo);}
    private void stopDemo(){boolean wasRunning=demo;demo=false;handler.removeCallbacks(demoStep);if(wasRunning&&demoButton!=null)updateDemoButton();}
    private void requestBlePermission(){if(Build.VERSION.SDK_INT>=31)requestPermissions(new String[]{Manifest.permission.BLUETOOTH_SCAN,Manifest.permission.BLUETOOTH_CONNECT},REQUEST_BLE);else requestPermissions(new String[]{Manifest.permission.ACCESS_FINE_LOCATION},REQUEST_BLE);}
    private boolean hasBlePermission(){if(Build.VERSION.SDK_INT>=31)return checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN)==PackageManager.PERMISSION_GRANTED&&checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT)==PackageManager.PERMISSION_GRANTED;return checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION)==PackageManager.PERMISSION_GRANTED;}
    @Override public void onRequestPermissionsResult(int req,String[] p,int[] g){super.onRequestPermissionsResult(req,p,g);if(req==REQUEST_BLE){if(hasBlePermission())state.setVisibility(View.GONE);else showConnectionMessage(getString(R.string.permission_denied));}}
    private void markDraft(){draftRevision++;setCanvasStatus(R.string.draft,false,false);}
    private void sendPreset(int opcode){grid.setPixels(PresetPatterns.pixels(opcode));markDraft();sendValue(MatrixProtocol.preset(opcode),"TX ");}
    private void sendRaw(){sendValue(MatrixProtocol.raw(grid.copyPixels()),"TX ");}
    private boolean sendValue(byte[] value,String prefix){
        if(!ready){toast(getString(R.string.connect_first));return false;}
        setCanvasStatus(R.string.sending,true,false);
        return commands.offer(value,draftRevision,prefix);
    }
    private void runDemoStep(){if(!demo)return;int op=new int[]{1,2,3,0}[demoIndex++%4];grid.setPixels(PresetPatterns.pixels(op));markDraft();if(!sendValue(MatrixProtocol.preset(op),"DEMO TX "))stopDemo();}
    private void toast(String s){Toast.makeText(this,s,Toast.LENGTH_SHORT).show();}
    private void log(String s){activityLog.append(s).append("\n");if(log!=null)log.append(s+"\n");}
    @Override public void onFound(BluetoothDevice d,String name){runOnUiThread(()->{
        String key=d.getAddress();
        if(name.equals(deviceNames.get(key)))return;
        String selectedKey=selectedDeviceKey();
        boolean hadPreferred=deviceNames.containsValue(PREFERRED_DEVICE_NAME);
        devices.put(key,d);deviceNames.put(key,name);
        ArrayList<Map.Entry<String,BluetoothDevice>> ordered=new ArrayList<>(devices.entrySet());
        Collections.sort(ordered,(a,b)->Boolean.compare(
            !PREFERRED_DEVICE_NAME.equals(deviceNames.get(a.getKey())),
            !PREFERRED_DEVICE_NAME.equals(deviceNames.get(b.getKey()))));
        // Use explicit address keys so the placeholder can never connect to a real device.
        deviceLabels.clear();deviceKeys.clear();
        boolean hasPreferred=deviceNames.containsValue(PREFERRED_DEVICE_NAME);
        if(!hasPreferred){deviceLabels.add(getString(R.string.select_device));deviceKeys.add(null);}
        for(Map.Entry<String,BluetoothDevice> entry:ordered){
            deviceKeys.add(entry.getKey());
            deviceLabels.add(deviceNames.get(entry.getKey())+"  ·  "+entry.getKey());
        }
        int nextSelection=!hadPreferred&&hasPreferred?0:deviceKeys.indexOf(selectedKey);
        picker.setContentDescription(getString(R.string.device_accessibility));deviceAdapter.notifyDataSetChanged();
        picker.setSelection(Math.max(0,nextSelection));updateConnectionControls();
    });}
    @Override public void onState(String s){runOnUiThread(()->{
        scanning=s.startsWith("Scanning");
        if(s.startsWith("Connecting")){linked=false;ready=false;connecting=true;}
        else if(s.startsWith("Connected")||s.startsWith("Subscribing")||s.startsWith("Reading")){linked=true;ready=false;connecting=true;}
        else if(s.equals("Ready")){linked=true;ready=true;connecting=false;commands.setReady(true);}
        else if(s.startsWith("Disconnected")){linked=false;ready=false;connecting=false;commands.setReady(false);stopDemo();setCanvasStatus(R.string.draft,false,false);}
        state.setVisibility(View.GONE);
        if(scanning)setDevicePlaceholder(getString(R.string.select_device));
        if(s.equals("Scan stopped")&&devices.isEmpty()){setDevicePlaceholder(getString(R.string.no_devices_found));picker.setContentDescription(getString(R.string.device_not_found_accessibility));}
        connectionBadge.setText(ready?R.string.ready:connecting?R.string.connecting:linked?R.string.connected:scanning?R.string.scanning:R.string.offline);
        connectionBadge.setActivated(ready);connectionBadge.setSelected(false);
        updateConnectionControls();log(s);
    });}
    @Override public void onStatus(String value,boolean fromRead){runOnUiThread(()->{log("RX "+value+(fromRead?" (read)":""));});}
    @Override public void onCommand(byte[] value,String result,boolean success){runOnUiThread(()->{
        log("ACK "+result+" for "+MatrixProtocol.hex(value));
        LatestCommandSender.Command completed=commands.current();
        if(completed==null||!Arrays.equals(completed.value,value))return;
        if(!success){stopDemo();setCanvasStatus(R.string.failed,false,true);toast(getString(R.string.command_failed,result));}
        else{if(completed.revision==draftRevision){setCanvasStatus(R.string.sent,true,false);}if(demo)handler.postDelayed(demoStep,1000);}
        commands.complete(success);
    });}
    @Override public void onError(String message){runOnUiThread(()->{stopDemo();ready=false;connecting=false;scanning=false;commands.setReady(false);setCanvasStatus(R.string.failed,false,true);showConnectionMessage(message);connectionBadge.setText(R.string.attention);connectionBadge.setActivated(false);connectionBadge.setSelected(true);updateConnectionControls();log("ERROR "+message);toast(message);});}
    @Override protected void onDestroy(){stopDemo();if(commands!=null)commands.setReady(false);handler.removeCallbacksAndMessages(null);if(session!=null)session.close();super.onDestroy();}
}
