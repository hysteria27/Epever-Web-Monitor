class RealTimeChart {
    /**
     * @param {string} canvasId - ID dari elemen canvas di HTML
     * @param {string} label - Nama dataset (misal: "Battery Voltage")
     * @param {string} color - Warna garis (hex atau rgb)
     * @param {string} unit - Satuan (misal: "V", "A", "W")
     */
    constructor(canvasId, label, color, unit) {
        this.canvas = document.getElementById(canvasId);
        this.maxDataPoints = 24; // Batas jumlah data yang tampil agar grafik berjalan

        this.gradient = this.canvas.getContext('2d').createLinearGradient(0, 0, 0, this.canvas.height);
        this.gradient.addColorStop(0, color.replace(')', ', 0.4)').replace('rgb', 'rgba'));
        this.gradient.addColorStop(1, color.replace(')', ', 0.1)').replace('rgb', 'rgba'));
        
        this.chart = new Chart(this.canvas, {
            type: 'line',
            data: {
                labels: [], // Label waktu kosong saat awal
                datasets: [{
                    label: label,
                    data: [],
                    borderColor: color,
                    backgroundColor: this.gradient,
                    borderWidth: 2,
                    tension: 0.4,
                    pointRadius: 1,
                    pointhoverRadius: 0,
                    fill: true
                }]
            },
            options: {
                responsive: true,
                maintainAspectRatio: false,
                animation: false, // Matikan animasi agar performa ringan saat update cepat
                interaction: {
                    mode: 'index',
                    intersect: false,
                },
                Plugins: {
                    legend: { display: false },
                    tooltip: {
                                backgroundColor: 'rgba(15, 23, 42, 0.9)',
                                titleColor: '#f8fafc',
                                bodyColor: '#f8fafc',
                                padding: 10,
                                cornerRadius: 8,
                                displayColors: false
                            }
                },
                scales: {
                    y: {
                        beginAtZero: true,
                        title: { display: true, text: unit }
                    },
                    x: {
                        title: { display: true, text: 'Time' },
                        display: false
                    }
                }
            }
        });
    }

    /**
     * @param {Array} labelsArray - Array string untuk sumbu X (Waktu)
     * @param {Array} dataArray - Array angka untuk sumbu Y (Nilai)
     */
    update(labelsArray, dataArray) {
        // Ambil hanya 12 data TERAKHIR dari array
        // .slice(-12) artinya ambil 12 item dari belakang
        const limitedLabels = labelsArray.slice(-this.maxDataPoints);
        const limitedData   = dataArray.slice(-this.maxDataPoints);

        // Timpa data lama dengan array baru
        this.chart.data.labels = limitedLabels;
        this.chart.data.datasets[0].data = limitedData;
        
        // Render ulang grafik
        this.chart.update();
    }
    
    /**
     * Method opsional untuk mereset grafik
     */
    reset() {
        this.chart.data.labels = [];
        this.chart.data.datasets.forEach((dataset) => {
            dataset.data = [];
        });
        this.chart.update();
    }
}