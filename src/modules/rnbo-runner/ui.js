// RNBO Runner — Overtake module

const RNBO_DIR = '/data/UserData/rnbo';
const CONTROL_BIN = RNBO_DIR + '/bin/rnbomovecontrol';
const SHADOW_CONFIG = RNBO_DIR + '/config/control-startup-shadow.json';
const DISPLAY_CTL = RNBO_DIR + '/scripts/display_ctl';
const HOOKS_DIR = '/data/UserData/schwung/hooks';

let phase = 0;
let displayEnabled = false;

globalThis.init = function() {
    phase = 0;
    displayEnabled = false;
};

globalThis.tick = function() {
    phase++;

    // Frame 2: install exit cleanup hook
    if (phase === 2) {
        host_system_cmd('mkdir -p ' + HOOKS_DIR);
        host_system_cmd('cp /data/UserData/schwung/modules/overtake/rnbo-runner/exit-hook.sh ' + HOOKS_DIR + '/overtake-exit.sh');
        host_system_cmd('chmod +x ' + HOOKS_DIR + '/overtake-exit.sh');
    }

    // Frame 3: launch rnbomovecontrol
    if (phase === 3) {
        clear_screen();
        print(0, 10, 'RNBO Runner', 2);
        print(0, 35, 'Loading...', 1);
        host_system_cmd('sh -c "export HOME=/data/UserData LD_LIBRARY_PATH=' + RNBO_DIR + '/lib:$LD_LIBRARY_PATH && nohup ' + CONTROL_BIN + ' -s ' + SHADOW_CONFIG + ' > /tmp/schwung-rnbomovecontrol.log 2>&1 &"');
    }

    // Frame 100: enable RNBO display
    if (phase === 100 && !displayEnabled) {
        clear_screen();
        host_system_cmd('sh -c "' + DISPLAY_CTL + ' 1"');
        displayEnabled = true;
    }

    // Keep clearing so overtake display doesn't override RNBO
    if (displayEnabled) {
        clear_screen();
    }
};

globalThis.onMidiMessageInternal = function(data) {};
globalThis.onMidiMessageExternal = function(data) {};
