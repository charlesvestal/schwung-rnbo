// RNBO Runner — Overtake module for running the official RNBO stack
//
// Launches jackd (shadow driver) + rnbooscquery + rnbomovecontrol alongside Move.
// rnbomovecontrol takes over Move's display and MIDI for the full RNBO experience.
// Audio from RNBO patches is mixed into Move's output via the JACK shadow bridge.

const RNBO_DIR = '/data/UserData/rnbo';
const JACK_BIN = RNBO_DIR + '/bin/jackd';
const RUNNER_BIN = RNBO_DIR + '/bin/rnbooscquery';
const CONTROL_BIN = RNBO_DIR + '/bin/rnbomovecontrol';
const LINK_BIN = RNBO_DIR + '/bin/jack_transport_link';
const PANEL_BIN = RNBO_DIR + '/bin/rnbo-runner-panel';
const PANEL_WWW = RNBO_DIR + '/share/rnbo-runner-panel/www/';
const PANEL_TPL = RNBO_DIR + '/share/rnbo-runner-panel/templates/';
const PID_DIR = '/tmp/schwung-rnbo';

let launched = false;
let initFrame = 0;
let exitRequested = false;

function isProcessRunning(name) {
    const result = host_system_cmd('pgrep -f ' + name + ' > /dev/null 2>&1 && echo yes || echo no');
    return result && result.trim() === 'yes';
}

function startJackStack() {
    // Ensure PID directory exists
    host_system_cmd('mkdir -p ' + PID_DIR);

    // Set LD_LIBRARY_PATH for all RNBO binaries
    const ldPath = 'export LD_LIBRARY_PATH=' + RNBO_DIR + '/lib:$LD_LIBRARY_PATH && ';

    // Start JACK if not running
    if (!isProcessRunning('jackd.*shadow')) {
        host_system_cmd(ldPath + JACK_BIN + ' -d shadow > /tmp/schwung-jack.log 2>&1 &');
        // Wait for JACK to initialize
        host_system_cmd('sleep 1');
    }

    // Start RNBO runner if not running
    if (!isProcessRunning('rnbooscquery')) {
        host_system_cmd(ldPath + RUNNER_BIN + ' --wait-for-audio --delay-start > /tmp/schwung-rnbooscquery.log 2>&1 &');
    }

    // Start Link transport bridge if not running
    if (!isProcessRunning('jack_transport_link')) {
        host_system_cmd(ldPath + LINK_BIN + ' > /dev/null 2>&1 &');
    }

    // Start web panel if not running
    if (!isProcessRunning('rnbo-runner-panel')) {
        host_system_cmd(ldPath + PANEL_BIN +
            ' --static-dir ' + PANEL_WWW +
            ' --template-dir ' + PANEL_TPL +
            ' > /dev/null 2>&1 &');
    }
}

function startControl() {
    if (!isProcessRunning('rnbomovecontrol')) {
        const ldPath = 'export LD_LIBRARY_PATH=' + RNBO_DIR + '/lib:$LD_LIBRARY_PATH && ';
        host_system_cmd(ldPath + CONTROL_BIN + ' > /tmp/schwung-rnbomovecontrol.log 2>&1 &');
    }
}

function stopControl() {
    host_system_cmd('pkill -f rnbomovecontrol 2>/dev/null');
}

// === Overtake module callbacks ===

globalThis.init = function() {
    clear_screen();
    print(10, 30, 'RNBO Runner', 2);
    print(10, 55, 'Starting...', 1);
    initFrame = 0;
    launched = false;
    exitRequested = false;
};

globalThis.tick = function() {
    initFrame++;

    if (!launched && initFrame === 2) {
        // Frame 2: start JACK stack (may already be running)
        startJackStack();
    }

    if (!launched && initFrame === 15) {
        // Frame 15 (~350ms): enable JACK display override
        shadow_set_param(0, 'jack:display', '1');
    }

    if (!launched && initFrame === 20) {
        // Frame 20 (~450ms): launch rnbomovecontrol
        startControl();
        launched = true;
    }

    if (launched) {
        // rnbomovecontrol handles all display/MIDI from here
        // We just idle until exit is requested
    }

    if (exitRequested) {
        // Clean exit: stop rnbomovecontrol, disable JACK display
        stopControl();
        shadow_set_param(0, 'jack:display', '0');
        host_exit_module();
    }
};

globalThis.onMidiMessageInternal = function(data) {
    // All MIDI goes to rnbomovecontrol via JACK — we don't need to handle it
    // The shim forwards cable-0 MIDI to shared memory → JACK → rnbomovecontrol
};

globalThis.onMidiMessageExternal = function(data) {
    // External MIDI also goes through JACK
};
