class ButtonHandler {
    constructor() {
        this.readParameters = document.getElementById('readParameters');
        this.saveParameters = document.getElementById('saveParameters');

        this.initListeners();
    }

    initListeners() {
        if (this.readParameters) {
            this.readParameters.addEventListener('click', () => {
                console.log("Read clicked");
                this.readParameters.classList.add('opacity-50', 'cursor-not-allowed');
                
                // Send flag to ESP32 to read Modbus
                firebase.database().ref('epever/parameters/isRequested').set(true)
                    .then(() => {
                        // Visual feedback
                        setTimeout(() => {
                            this.readParameters.classList.remove('opacity-50', 'cursor-not-allowed');
                        }, 2000); // Re-enable after 2s
                        initSettingsListener();
                    });
            });
        }

        if (this.saveParameters) {
            this.saveParameters.addEventListener('click', (event) => {
                event.preventDefault(); // Prevent default form submission behavior (stopping refresh the page)
                console.log("Save clicked");
                // Your save logic here
            });
        }
    }
}

// Usage: Ensure the DOM is ready before creating the class
document.addEventListener('DOMContentLoaded', () => {
    new ButtonHandler();
});

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
        if (diff > 60) {
            manageStatusDot('device-conn-dot', false);
        } else {
            manageStatusDot('device-conn-dot', true);
        }
    }, 5000); // Cek setiap 5 detik
});