if (!firebase.apps.length) firebase.initializeApp(firebaseConfig);
const db = firebase.database();
const storage = firebase.storage();
const auth = firebase.auth();

let isAuthenticated = false;    // User authentication state
let isConnectionActive = false; // Prevents double-starting listeners
let idleTimer = null;           // Holds the countdown
const IDLE_TIMEOUT = 60000;     // 1 minute in milliseconds

const activityEvents = ['mousedown', 'mousemove', 'keydown', 'scroll', 'touchstart'];   // Events that count as "Activity"
activityEvents.forEach(event => {
    document.addEventListener(event, resetIdleTimer, { passive: true });                // Attach activity listeners
});
// resetIdleTimer();   // Start on load

let liveChartInstance = null;
let dayChartInstance = null;
let livePVData = Array(30).fill(0);
let liveLabels = Array(30).fill('');
let fetchedLogData = [];
let lastPacketTime = Date.now(); // To track device timeouts

window.addEventListener('load', () => {
    lucide.createIcons();
    initCharts();
    // auth.signInAnonymously().catch(console.error);
    // startFirebaseListener();
    
    document.getElementById('logDateSelector').valueAsDate = new Date(); // Set to today by default
    
    // Setup Drag & Drop
    const dropzone = document.getElementById('upload-dropzone');
    dropzone.addEventListener('click', () => document.getElementById('fw-file-input').click());
    dropzone.addEventListener('dragover', (e) => { e.preventDefault(); e.currentTarget.classList.add('bg-gray-100'); });
    dropzone.addEventListener('dragleave', (e) => { e.currentTarget.classList.remove('bg-gray-100'); });
    dropzone.addEventListener('drop', handleDrop);

    setInterval(() => {
        const now = Date.now();
        const diff = (now - lastPacketTime) / 1000; // Selisih dalam detik
        const devStatus = document.getElementById('device-status');
        const devDot = document.getElementById('conn-dot-device');
        if (diff > 120) { // Jika lebih dari 2 menit
            devStatus.textContent = "Device Offline";
            devStatus.className = "text-sm text-red-600 font-bold";
            devDot.className = "inline-block w-2 h-2 rounded-full bg-red-500 animate-pulse";
            if (firebase.auth().currentUser) {db.ref('epever/is_online').set(false).catch(e => {});}
        }
    }, 5000); // Cek setiap 5 detik
});

// --- AUTH LOGIC ---
auth.onAuthStateChanged((user) => {
    const loadingScreen = document.getElementById('view-loading');
    
    if (user) {
        document.getElementById('view-login').classList.add('hidden-view');
        document.getElementById('view-app').classList.remove('hidden-view');
        document.getElementById('view-app').classList.add('flex');
        
        startFirebaseListener();
        isAuthenticated = true;
    } else {
        document.getElementById('view-app').classList.add('hidden-view');
        document.getElementById('view-app').classList.remove('flex');
        document.getElementById('view-login').classList.remove('hidden-view');
        
        document.getElementById('login-btn-text').textContent = "Sign In";
        stopFirebaseListener();
    }

    // 2. Terakhir, hilangkan Loading Screen setelah keputusan dibuat
    // Kita kasih sedikit delay/animasi agar transisi halus (opsional)
    if (loadingScreen && !loadingScreen.classList.contains('hidden-view')) {
        setTimeout(() => {
            loadingScreen.classList.add('opacity-0'); // Efek fade out (jika didukung CSS)
            setTimeout(() => {
                loadingScreen.classList.add('hidden-view');
            }, 300); // Waktu tunggu animasi fade out
        }, 500); // Delay minimal agar loading tidak cuma kedip
    }
});

function doLogin() {
    const email = document.getElementById('login-email').value;
    const pass = document.getElementById('login-pass').value;

    if (!email || !pass) {
        alert("Email dan Password tidak boleh kosong");
        return;
    }

    const btn = document.getElementById('login-btn-text');
    const err = document.getElementById('login-error');
    
    btn.textContent = "Signing In...";
    err.classList.add('hidden');

    auth.signInWithEmailAndPassword(email, pass)
        .catch((error) => {
            btn.textContent = "Sign In";
            err.textContent = "Invalid Login: " + error.message;
            err.classList.remove('hidden');
        });
}

function doLogout() {
    auth.signOut();
    isAuthenticated = false;
}

// --- IDLE & VISIBILITY LOGIC ---
function resetIdleTimer() {
    if (!isAuthenticated) return;                      // No need to track if not logged in

    // 1. Clear existing timer
    if (idleTimer) clearTimeout(idleTimer);

    // 2. If we were paused/stopped and the tab is visible, wake up!
    if (!isConnectionActive && !document.hidden) {
        startFirebaseListener();
    }

    // 3. Set new timer: If no activity for 60s, Stop.
    idleTimer = setTimeout(() => {
        if (!document.hidden) { // Only log if we are looking at it
            console.log("User inactive for 1 min. Disconnecting.");
        }
        stopFirebaseListener();
    }, IDLE_TIMEOUT);
}

// Handle Tab Switching / Minimize
/* document.addEventListener("visibilitychange", () => {
    if (document.hidden && isAuthenticated) {
        // Immediately kill connection when tab is hidden
        stopFirebaseListener(); 
    } else if (!document.hidden && isAuthenticated) {
        // Immediately restore connection when tab comes back
        startFirebaseListener();
        resetIdleTimer();
    } else {
        // Tab is visible but not authenticated
        stopFirebaseListener();
    }
}); */

// Safety net: Ensures disconnection if the user closes the window/tab directly
window.addEventListener("pagehide", () => {
    if (isAuthenticated) {
        stopFirebaseListener();
    }
});

function switchView(id) {
    document.getElementById('view-dashboard').className = id==='dashboard'?'fade-in space-y-6':'hidden-view';
    document.getElementById('view-history').className = id==='history'?'fade-in space-y-6':'hidden-view';
    document.getElementById('view-firmware').className = id==='firmware'?'fade-in space-y-6':'hidden-view';
    
    // Update Buttons
    const activeClass = "px-4 py-2 rounded-md text-sm font-medium flex gap-2 items-center bg-solar-500 text-white transition-all shadow-md";
    const inactiveClass = "px-4 py-2 rounded-md text-sm font-medium flex gap-2 items-center bg-white text-gray-600 hover:bg-gray-100 transition-all shadow-md";

    if(id==='history') fetchDayLog();               // Auto fetch log when switched to history view
    if(id==='firmware') fetchFirmwareInfo();        // Auto fetch firmware info when switched to firmware view
    
    document.getElementById('btn-dashboard').className  = `${id==='dashboard'   ?activeClass:inactiveClass}`;
    document.getElementById('btn-history').className    = `${id==='history'     ?activeClass:inactiveClass}`;
    document.getElementById('btn-firmware').className   = `${id==='firmware'    ?activeClass:inactiveClass}`;
}

function startFirebaseListener() {
    if (isConnectionActive) return;         // Prevent double connections

    console.log("Starting Connection...");
    firebase.database().goOnline(); // Ensure socket is open

    const devStatus = document.getElementById('device-status');
    const devDot = document.getElementById('conn-dot-device');
    const statusEl = document.getElementById('connection-status');
    const dotEl = document.getElementById('conn-dot');

    // --- TAMBAHAN LISTENER STATUS PERANGKAT ---
    db.ref('epever/is_online').on('value', (snapshot) => {
        const isDeviceOnline = snapshot.val(); // true atau false
        
        // Update UI Dot Indikator Utama
        if (isDeviceOnline === true) {
            devStatus.textContent = "Device Online";
            devStatus.className = "text-sm text-green-600 font-bold";
            devDot.className = "inline-block w-2 h-2 rounded-full bg-green-500 animate-pulse";
        } else {
            devStatus.textContent = "Device Offline";
            devStatus.className = "text-sm text-red-600 font-bold";
            devDot.className = "inline-block w-2 h-2 rounded-full bg-red-500 animate-pulse";
        }
    });
    // --- AKHIR TAMBAHAN ---
    
    db.ref('epever/live').on('value', (snapshot) => {
        const data = snapshot.val();
        if (!data) return;

        // Update UI only if we are actually connected
        statusEl.textContent = "Firebase Connected";
        dotEl.className = "inline-block w-2 h-2 rounded-full bg-solar-500 animate-pulse";
        updateUI(data);
    });
    isConnectionActive = true;
}

function stopFirebaseListener() {
    if (!isConnectionActive) return;

    console.log("Stopping Connection (Idle/Background)...");

    // 1. Remove listeners to stop callbacks
    db.ref('epever/live').off();

    // 2. Kill the WebSocket to save bandwidth/battery immediately
    firebase.database().goOffline();

    const statusEl = document.getElementById('connection-status');
    const dotEl = document.getElementById('conn-dot');
    statusEl.textContent = "Paused (Idle)"; 
    dotEl.className = "inline-block w-2 h-2 rounded-full bg-yellow-500";

    isConnectionActive = false;
}

function updateUI(data) {
    lastPacketTime = Date.now(); // reset when packet received

    const statusLabel = getStatusLabel(data.status_code);
    const statusEl = document.getElementById('charging-stage-display');
    statusEl.textContent = statusLabel.text;
    statusEl.className = `text-lg font-bold mt-1 ${statusLabel.color}`;

    document.getElementById('panel-power').textContent = data.pv.power.toFixed(2);
    document.getElementById('pv-bar').style.width = Math.min((data.pv.power/500)*100, 100) + "%";
    document.getElementById('battery-voltage').textContent = data.batt.volt.toFixed(2);
    document.getElementById('load-power').textContent = data.load.power.toFixed(2);
    document.getElementById('gauge-soc-text').textContent = Math.round(data.batt.soc) + "%";
    document.getElementById('detail-pv-volt').textContent = data.pv.volt.toFixed(2);
    document.getElementById('detail-chg-amp').textContent = data.batt.amps.toFixed(2);
    document.getElementById('detail-temp').textContent = data.temp.toFixed(1);
    document.getElementById('detail-daily-energy').textContent = data.daily_kwh.toFixed(2);
    
    const offset = 282.7 - ((data.batt.soc/100) * 282.7);
    const ring = document.querySelector('.gauge-ring');
    if(ring) { ring.style.strokeDashoffset = offset; ring.style.stroke = data.batt.soc < 30 ? "#ef4444" : "#22c55e"; }
    else console.error("Gauge ring element not found");

    if(liveChartInstance){
        livePVData.push(data.pv.power); livePVData.shift(); liveChartInstance.update('none');
    }

    // Cek selisih waktu sekarang vs waktu data terakhir (timestamp dari ESP32)
    const now = Date.now() / 1000; // Detik sekarang
    const dataTime = data.timestamp || 0; 
    const diff = now - dataTime;

    // Kalau data telat lebih dari 60 detik, anggap hang. 120 detik, set is_online ke false di database.
    if (diff > 120) {
        document.getElementById('device-status').textContent = "Device Offline";
        document.getElementById('device-status').className = "text-sm text-red-600 font-bold";
        document.getElementById('conn-dot-device').className = "inline-block w-2 h-2 rounded-full bg-red-500 animate-pulse";
        db.ref('epever/is_online').set(false)
            .catch((error) => console.error("Gagal update DB:", error));
    } else if (diff > 60) {
        document.getElementById('device-status').textContent = "Device Probably Offline";
        document.getElementById('device-status').className = "text-sm text-orange-600 font-bold";
        document.getElementById('conn-dot-device').className = "inline-block w-2 h-2 rounded-full bg-orange-500 animate-bounce";
    } else {
        document.getElementById('device-status').textContent = "Device Online";
        document.getElementById('device-status').className = "text-sm text-green-600 font-bold";
        document.getElementById('conn-dot-device').className = "inline-block w-2 h-2 rounded-full bg-green-500 animate-pulse";
        db.ref('epever/is_online').set(true)
            .catch((error) => console.error("Gagal update DB:", error));
    }
}

function initCharts() {
    const ctx = document.getElementById('liveChart');
    if(!ctx) return;

    // Create Gradient
    const gradient = ctx.getContext('2d').createLinearGradient(0, 0, 0, 300);
    gradient.addColorStop(0, 'rgba(14, 165, 233, 0.5)');
    gradient.addColorStop(1, 'rgba(14, 165, 233, 0.0)');

    liveChartInstance = new Chart(
        ctx, { 
            type: 'line', 
            data: { 
                labels: liveLabels, 
                datasets: [{ 
                    label: 'PV (W)', 
                    data: livePVData, 
                    borderwidth: 2,
                    borderColor: '#0ea5e9', 
                    backgroundColor: gradient, 
                    fill: true, 
                    tension: 0.4, // Smooth curves
                    pointRadius: 0,
                    pointHoverRadius: 6 }] 
                }, 
                options: { 
                    responsive: true, 
                    maintainAspectRatio: false, 
                    plugins: { 
                        legend: {display:false} 
                    }, 
                    scales: { 
                        x: { display: false }, 
                        y: { beginAtZero: true } 
                    } 
                } 
            });
}

// --- Helper to Decode Epever Status Code ---
// Extract Bits 3-2 for Charging Status
function getStatusLabel(code) {
    if(code === undefined || code === null) return "-";
    const chgState = (code >> 2) & 0x03;
    const voltState = (code >> 14) & 0x03
    if(voltState===1)return{text:"OVP",color:"text-red-500"};
    if(code&0x8000)return{text:"Fault",color:"text-red-600"};;
    switch(chgState) {
        case 0: return{text:"Standby",color:"text-gray-400"};
        case 1: return{text:"Float",color:"text-solar-500"};
        case 2: return{text:"Boost",color:"text-orange-500"};
        case 3: return{text:"Equalize",color:"text-blue-500"};
        default: return{text:"Active",color:"text-green-500"};
    }
}

// --- DAY CHART MANAGEMENT ---
function getDayChartInstance(chartLabels = [], chartData = []) {
    const ctxDay = document.getElementById('dayChart');
    if (!ctxDay) {
        console.warn("Chart skipped: canvas element not found");
        return;
    }

    if (!dayChartInstance) {
        try {
            dayChartInstance = new Chart(ctxDay, { 
                type: 'bar', 
                data: { labels: [], datasets: [{ label: 'Power (W)', data: [], backgroundColor: '#f97316' }] }, 
                options: { responsive: true, maintainAspectRatio: false } 
            });
        } catch (e) {
            console.error("Chart init failed:", e);
            return;
        }
    }

    const chart = dayChartInstance;
    if (chart) {
        chart.data.labels = chartLabels;
        chart.data.datasets[0].data = chartData;
        chart.update();
    } else {
        console.warn("Chart skipped: could not initialize canvas");
    }
}

// --- HISTORY LOG FETCHING ---
function fetchDayLog() {
    const dateInput = document.getElementById('logDateSelector').value;
    if(!dateInput) return alert("Select a date");

    const startTs = new Date(dateInput).getTime() / 1000;       // Start of selected day in seconds
    const endTs = startTs + 86400;                              // End of selected day in seconds

    const tbody = document.getElementById('detail-table-body');
    tbody.innerHTML = '<tr><td colspan="4" class="p-4 text-center">Loading...</td></tr>';

    db.ref('epever/history').orderByChild('hStamp').startAt(startTs).endAt(endTs).once('value')
    .then(snapshot => {
        const data = snapshot.val();
        fetchedLogData = []; 

        let html = '';
        let chartLabels = [];
        let chartData = [];
        
        if(!data) {
            tbody.innerHTML = '<tr><td colspan="4" class="p-4 text-center">No data found for this date</td></tr>';
            getDayChartInstance(chartLabels, chartData); // Clear chart
            return;
        }

        const logs = Object.values(data).sort((a,b) => b.hStamp - a.hStamp);

        logs.forEach(log => {
            const dateObj = new Date(log.hStamp * 1000);    // Convert to milliseconds
            const timeStr = dateObj.toLocaleTimeString([], {hour: '2-digit', minute:'2-digit'}); // Format HH:MM
            const statusStr = getStatusLabel(log.hCCode); // Decode 'hCCode' from DB
            const statusStrEL = `<span class="px-2 py-1 rounded text-xs font-bold bg-gray-100 ${statusStr.color}">${statusStr.text}</span>`;
            fetchedLogData.push({ time: timeStr, ...log }); 

            html += `<tr class="hover:bg-gray-50 dark:hover:bg-slate-800">
                <td class="px-6 py-4">${timeStr}</td>
                <td class="px-6 py-4">${statusStrEL}</td>
                <td class="px-6 py-4 text-right">${log.hPWatt} W</td>
                <td class="px-6 py-4 text-right">${log.hBVolt} V</td>
                <td class="px-6 py-4 text-right">${log.hBSOC}%</td>
            </tr>`;

            chartLabels.push(timeStr);
            chartData.push(log.hPWatt);
        });
        tbody.innerHTML = html;
        chartLabels.reverse();
        chartData.reverse();

        getDayChartInstance(chartLabels, chartData); // Update chart
    })
    .catch(err => {
        console.error(err);
        tbody.innerHTML = `<tr><td colspan="4" class="p-4 text-center text-red-500">Error: ${err.message}</td></tr>`;
    });
}

// --- DOWNLOAD LOG AS CSV ---
function downloadLog() {
    if(fetchedLogData.length === 0) return alert("No data to download. Search for a date first.");
    
    let csvContent = "data:text/csv;charset=utf-8,Time,Status,Power(W),Battery(V),SOC(%)\n";
    fetchedLogData.forEach(row => {
        const stCode = getStatusLabel(row.hCCode); // Decode 'hCCode' from DB
        csvContent += `${row.time},${stCode},${row.hPWatt},${row.hBVolt},${row.hBSOC}\n`;
    });

    const encodedUri = encodeURI(csvContent);
    const link = document.createElement("a");
    link.setAttribute("href", encodedUri);
    link.setAttribute("download", "solar_log.csv");
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
}

async function fetchMonthlySummary() {
    try {
    } catch (e) {
        console.error("Error fetching monthly summary:", e);
    }
}

// --- FIRMWARE INFO FETCHING ---
function fetchFirmwareInfo() {
    db.ref('/epever/firmware_info').once('value').then(snapshot => {
        const info = snapshot.val();
        if (!info) return alert("No firmware info available.");
        document.getElementById('fw-version').textContent = info.firmware_version || '--';
        document.getElementById('fw-date').textContent = info.firmware_date || '--';
        document.getElementById('fw-chip').textContent = info.chip_model || '--';
        document.getElementById('fw-free').textContent = info.free_space ? info.free_space + ' bytes' : '--';
    }).catch(e => alert("Error fetching firmware info: " + e.message));
}

// --- FIRMWARE UPLOAD LOGIC ---
let selectedFile = null;
function handleDrop(e) {
    e.preventDefault();
    e.stopPropagation();
    e.currentTarget.classList.remove('bg-gray-100');
    if (e.dataTransfer.files.length && e.dataTransfer.files[0].name.endsWith('.bin')) {
        selectedFile = e.dataTransfer.files[0];
        document.getElementById('fw-message').textContent = `Selected: ${selectedFile.name}`;
        document.getElementById('fw-message').className = 'text-xs text-center text-green-600 dark:text-green-400';
    } else {
        document.getElementById('fw-message').textContent = 'Please drop a .bin file';
        document.getElementById('fw-message').className = 'text-xs text-center text-red-600';
    }
}

function handleFileSelect(event) {
    const file = event.target.files[0];
    if (file && file.name.endsWith('.bin')) {
        selectedFile = file;
        document.getElementById('fw-message').textContent = `Selected: ${file.name}`;
        document.getElementById('fw-message').className = "text-xs text-center text-green-600 font-bold mt-2";
    } else {
        alert("Please select a .bin file!");
        selectedFile = null;
    }
}

function uploadFirmware() {
    if(!selectedFile) return alert("Select file first!");
    
    const btn = document.getElementById('fw-upload-btn');
    const statusDiv = document.getElementById('fw-upload-status');
    const statusText = document.getElementById('fw-status-text');
    const progressBar = document.getElementById('fw-progress-bar');
    
    btn.disabled = true;
    statusDiv.classList.remove('hidden');
    statusText.textContent = "Uploading to Cloud...";

    // 1. Upload ke Storage dengan nama "firmware.bin" (fixed name agar ESP32 mudah download)
    const storageRef = storage.ref('firmware.bin');
    const task = storageRef.put(selectedFile);

    task.on('state_changed', 
        (snapshot) => {
            const progress = (snapshot.bytesTransferred / snapshot.totalBytes) * 100;
            progressBar.style.width = progress + "%";
        }, 
        (error) => {
            console.error(error);
            alert("Upload Failed: " + error.message);
            btn.disabled = false;
        }, 
        () => {
            // 2. Upload Sukses -> Trigger ESP32
            statusText.textContent = "Flashing Device...";
            progressBar.style.width = "100%";
            
            // Set flag di database
            db.ref('epever/firmware_info/ota_trigger').set(true)
            .then(() => {
                alert("Success! Device will restart shortly.");
                btn.disabled = false;
                statusDiv.classList.add('hidden');
                selectedFile = null;
                document.getElementById('fw-message').textContent = "";
            })
            .catch(e => alert("DB Error: " + e.message));
        }
    );
}