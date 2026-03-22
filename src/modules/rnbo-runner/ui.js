// RNBO Runner — Overtake module

const RNBO_DIR = '/data/UserData/rnbo';
const CONTROL_BIN = RNBO_DIR + '/bin/rnbomovecontrol';
const SHADOW_CONFIG = RNBO_DIR + '/config/control-startup-shadow.json';
const DISPLAY_ON = RNBO_DIR + '/scripts/display-on.sh';
const JACK_CONNECT = '/tmp/jack_connect';

let launched = false;
let connected = false;
let initFrame = 0;

function launchRnbo() {
    host_system_cmd('sh -c "export HOME=/data/UserData LD_LIBRARY_PATH=' + RNBO_DIR + '/lib:$LD_LIBRARY_PATH && nohup ' + CONTROL_BIN + ' -s ' + SHADOW_CONFIG + ' > /tmp/schwung-rnbomovecontrol.log 2>&1 &"');
}

function isRnboRunning() {
    return host_system_cmd('sh -c "pgrep rnbomovecontrol >/dev/null 2>&1"') === 0;
}

function enableDisplay() {
    host_system_cmd('sh ' + DISPLAY_ON);
}

function connectPorts() {
    // Connect rnbomovecontrol's MIDI output to system MIDI playback (for LEDs)
    host_system_cmd('sh -c "LD_LIBRARY_PATH=' + RNBO_DIR + '/lib ' + JACK_CONNECT + ' move-control:midi_out system:midi_playback 2>/dev/null"');
    host_system_cmd('sh -c "LD_LIBRARY_PATH=' + RNBO_DIR + '/lib ' + JACK_CONNECT + ' move-control:midi_control_out system:midi_playback 2>/dev/null"');
    // Also ensure display is connected
    host_system_cmd('sh -c "LD_LIBRARY_PATH=' + RNBO_DIR + '/lib ' + JACK_CONNECT + ' move-control:display system:display 2>/dev/null"');
    // And MIDI input
    host_system_cmd('sh -c "LD_LIBRARY_PATH=' + RNBO_DIR + '/lib ' + JACK_CONNECT + ' system:midi_capture move-control:midi_in 2>/dev/null"');
}

globalThis.init = function() {
    initFrame = 0;
    launched = false;
    connected = false;

    if (isRnboRunning()) {
        enableDisplay();
        launched = true;
    }
};

globalThis.tick = function() {
    initFrame++;

    if (launched && !connected && initFrame > 5) {
        // Stack is running — force port connections after settle time
        connectPorts();
        connected = true;
        return;
    }

    // Retry connections periodically in case of XRun recovery
    if (launched && connected && (initFrame % 200) === 0) {
        connectPorts();
    }

    if (launched) return;

    clear_screen();
    print(0, 10, 'RNBO Runner', 2);

    if (initFrame === 3) {
        print(0, 35, 'Launching...', 1);
        launchRnbo();
    }

    // Poll every ~450ms
    if (initFrame > 10 && (initFrame % 20) === 0) {
        if (isRnboRunning()) {
            enableDisplay();
            launched = true;
        } else if (initFrame > 200) {
            print(0, 35, 'Failed to start', 1);
        } else {
            print(0, 35, 'Waiting... (' + initFrame + ')', 1);
        }
    }
};

globalThis.onMidiMessageInternal = function(data) {};
globalThis.onMidiMessageExternal = function(data) {};
