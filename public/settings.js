// --- CONFIGURATION MAPPING ---
// Maps HTML IDs to your Firebase Database keys (adjust keys to match your ESP32 code)
const paramMap = {
    // Battery Config
    'param-batt-type':  'batt_type',      // e.g., "Sealed", "User"
    'param-rated-v':    'system_voltage',    // e.g., "Auto", "12/24"
    'param-capacity':   'batt_capacity',          // e.g., 200
    'param-temp-comp':  'temp_compensation',         // e.g., 3

    // Charging Voltages
    'param-ovd':        'h_voltage_disconnect',    // Over Voltage Disconnect
    'param-cl':         'charging_limit_voltage',    // Charging Limit
    'param-ovr':        'overvoltage_reconnect',     // Over Voltage Reconnect
    'param-eq':         'equalization_voltage',   // Equalize Charge
    'param-boost':      'boost_voltage',      // Boost Charge
    'param-float':      'float_voltage',      // Float Charge
    'param-br':         'boost_reconnect_voltage',         // Boost Reconnect

    // Protection & Limits
    'param-lvr':        'low_voltage_reconnect',      // Low Voltage Reconnect
    'param-uv-rec':     'undervoltage_recover',    // Under Voltage Recover
    'param-uv-warn':    'undervoltage_warning',   // Under Voltage Warning
    'param-lvd':        'low_voltage_disconnect',     // Low Voltage Disconnect
    'param-dis-limit':  'discharge_limit_voltage',   // Discharge Limit

    // Durations
    'param-eq-time':    'equalize_duration', // Minutes
    'param-boost-time': 'boost_duration'     // Minutes
};

function batteryCodeToString(code) {
    switch(code) {
        case 0: return "User";
        case 1: return "Sealed";
        case 2: return "Gel";
        case 3: return "Flooded";
        default: return "Unknown";
    }
}  

// --- 1. SETUP LISTENER (READ DATA) ---
// Call this function when the app starts (e.g., inside your auth state change or init)
function initSettingsListener() {
    const db = firebase.database();
    const paramRef = db.ref('epever/parameters/data');

    paramRef.off(); // Remove previous listeners if any
    
    // Listening to 'epever/parameters/data' (Change this path to where your ESP32 writes settings)
    paramRef.on('value', (snapshot) => {
        const data = snapshot.val();
        if (data) {
            console.log("Settings received:", data);
            populateForm(data);
        }
    });
}

// Helper to fill the HTML inputs
function populateForm(data) {
    for (const [elementId, dbKey] of Object.entries(paramMap)) {
        const el = document.getElementById(elementId);
        if (el && data[dbKey] !== undefined) {
            if (elementId === 'param-batt-type') {
                // Convert battery type code to string for the drop-down
                el.value = batteryCodeToString(data[dbKey]);
            } else {
                el.value = data[dbKey];
            }
        }
    }
}

// --- 2. SAVE SETTINGS (WRITE DATA) ---
function saveSettings(event) {
    event.preventDefault(); // Stop page reload

    const db = firebase.database();
    const newSettings = {};
    let isValid = true;

    // Loop through map and get values from UI
    for (const [elementId, dbKey] of Object.entries(paramMap)) {
        const el = document.getElementById(elementId);
        if (el) {
            // Convert to float for numbers, keep strings for text
            const val = el.value;
            if (val === "") {
                isValid = false; // Basic validation
                el.classList.add('border-red-500'); // Highlight empty fields
            } else {
                el.classList.remove('border-red-500');
                // Check if it should be a number (if it's not the drop-downs)
                if (elementId !== 'param-batt-type' && elementId !== 'param-rated-v') {
                     newSettings[dbKey] = parseFloat(val);
                } else {
                     newSettings[dbKey] = val;
                }
            }
        }
    }

    if (!isValid) {
        alert("Please fill in all fields.");
        return;
    }

    // Button Loading State
    const btn = event.submitter; // The save button
    const originalText = btn.innerHTML;
    btn.innerHTML = `<i data-lucide="loader-2" class="w-4 h-4 animate-spin"></i> Saving...`;
    lucide.createIcons();

    // Write to a 'commands' path so ESP32 knows to update Epever
    // ESP32 should listen to '/epever/parameters/isSetRequested'
    db.ref('epever/parameters/isSetRequested').set(newSettings)
        .then(() => {
            showSaveStatus();
            btn.innerHTML = originalText;
        })
        .catch((error) => {
            console.error("Write error", error);
            alert("Error saving: " + error.message);
            btn.innerHTML = originalText;
        });
}

// --- 3. READ FROM DEVICE (COMMAND) ---
function readParameters() {
    /*const btn = document.querySelector('button[onclick="readParameters()"]');
    btn.classList.add('opacity-50', 'cursor-not-allowed');
    
    // Send flag to ESP32 to read Modbus
    firebase.database().ref('/epever/parameters/isRequested').set(true)
        .then(() => {
            // Visual feedback
            setTimeout(() => {
                btn.classList.remove('opacity-50', 'cursor-not-allowed');
            }, 2000); // Re-enable after 2s
        });*/
}

// Helper for the "Saved!" text animation
function showSaveStatus() {
    const statusEl = document.getElementById('save-status');
    statusEl.classList.remove('opacity-0');
    setTimeout(() => {
        statusEl.classList.add('opacity-0');
    }, 3000);
}