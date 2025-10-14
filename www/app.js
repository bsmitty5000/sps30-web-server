let ws = null;
let massChart, numberChart;
let maxDataPoints = 60; // Keep last 60 seconds of data

// Data storage (timestamps and readings)
let data = {
    timestamps: [],
    mc_1p0: [],
    mc_2p5: [],
    mc_4p0: [],
    mc_10p0: [],
    nc_0p5: [],
    nc_1p0: [],
    nc_2p5: [],
    nc_4p0: [],
    nc_10p0: [],
    typical_particle_size: []
};

// Chart color schemes
const massColors = {
    mc_1p0: 'rgba(255, 107, 107, 1)',
    mc_2p5: 'rgba(255, 165, 0, 1)',
    mc_4p0: 'rgba(100, 200, 255, 1)',
    mc_10p0: 'rgba(75, 192, 192, 1)'
};

const numberColors = {
    nc_0p5: 'rgba(255, 107, 107, 1)',
    nc_1p0: 'rgba(255, 165, 0, 1)',
    nc_2p5: 'rgba(100, 200, 255, 1)',
    nc_4p0: 'rgba(153, 102, 255, 1)',
    nc_10p0: 'rgba(75, 192, 192, 1)'
};

// Initialize charts on page load
window.addEventListener('DOMContentLoaded', () => {
    initCharts();
    populateTable();
});

function initCharts() {
    const massCtx = document.getElementById('massChart').getContext('2d');
    massChart = new Chart(massCtx, {
        type: 'line',
        data: {
            labels: data.timestamps,
            datasets: [
                createDataset('PM1.0', 'mc_1p0', massColors.mc_1p0),
                createDataset('PM2.5', 'mc_2p5', massColors.mc_2p5),
                createDataset('PM4.0', 'mc_4p0', massColors.mc_4p0),
                createDataset('PM10.0', 'mc_10p0', massColors.mc_10p0)
            ]
        },
        options: {
            responsive: true,
            maintainAspectRatio: true,
            interaction: {
                mode: 'index',
                intersect: false
            },
            plugins: {
                legend: {
                    position: 'top',
                }
            },
            scales: {
                y: {
                    beginAtZero: true,
                    title: {
                        display: true,
                        text: 'µg/m³'
                    }
                }
            }
        }
    });

    const numberCtx = document.getElementById('numberChart').getContext('2d');
    numberChart = new Chart(numberCtx, {
        type: 'line',
        data: {
            labels: data.timestamps,
            datasets: [
                createDataset('PM0.5', 'nc_0p5', numberColors.nc_0p5),
                createDataset('PM1.0', 'nc_1p0', numberColors.nc_1p0),
                createDataset('PM2.5', 'nc_2p5', numberColors.nc_2p5),
                createDataset('PM4.0', 'nc_4p0', numberColors.nc_4p0),
                createDataset('PM10.0', 'nc_10p0', numberColors.nc_10p0)
            ]
        },
        options: {
            responsive: true,
            maintainAspectRatio: true,
            interaction: {
                mode: 'index',
                intersect: false
            },
            plugins: {
                legend: {
                    position: 'top',
                }
            },
            scales: {
                y: {
                    beginAtZero: true,
                    title: {
                        display: true,
                        text: '#/cm³'
                    }
                }
            }
        }
    });
}

function createDataset(label, key, color) {
    return {
        label: label,
        data: data[key],
        borderColor: color,
        backgroundColor: color.replace('1)', '0.1)'),
        tension: 0.3,
        fill: false,
        pointRadius: 0,
        pointHoverRadius: 6,
        borderWidth: 2
    };
}

function connectToServer() {
    const url = document.getElementById('server-url').value;
    
    if (!url) {
        alert('Please enter a server URL');
        return;
    }

    ws = new WebSocket(url);

    ws.onopen = () => {
        console.log('Connected to server');
        updateConnectionStatus(true);
        document.getElementById('connect-btn').disabled = true;
        document.getElementById('disconnect-btn').disabled = false;
    };

    ws.onmessage = (event) => {
        try {
            const reading = JSON.parse(event.data);
            addDataPoint(reading);
        } catch (e) {
            console.error('Error parsing message:', e);
        }
    };

    ws.onerror = (error) => {
        console.error('WebSocket error:', error);
        alert('WebSocket error: ' + error);
    };

    ws.onclose = () => {
        console.log('Disconnected from server');
        updateConnectionStatus(false);
        document.getElementById('connect-btn').disabled = false;
        document.getElementById('disconnect-btn').disabled = true;
    };
}

function disconnectFromServer() {
    if (ws) {
        ws.close();
    }
}

function addDataPoint(reading) {
    const now = new Date().toLocaleTimeString();
    
    // Add timestamp and data
    data.timestamps.push(now);
    data.mc_1p0.push(reading.mc_1p0 || 0);
    data.mc_2p5.push(reading.mc_2p5 || 0);
    data.mc_4p0.push(reading.mc_4p0 || 0);
    data.mc_10p0.push(reading.mc_10p0 || 0);
    data.nc_0p5.push(reading.nc_0p5 || 0);
    data.nc_1p0.push(reading.nc_1p0 || 0);
    data.nc_2p5.push(reading.nc_2p5 || 0);
    data.nc_4p0.push(reading.nc_4p0 || 0);
    data.nc_10p0.push(reading.nc_10p0 || 0);
    data.typical_particle_size.push(reading.typical_particle_size || 0);

    // Keep only last maxDataPoints
    if (data.timestamps.length > maxDataPoints) {
        data.timestamps.shift();
        data.mc_1p0.shift();
        data.mc_2p5.shift();
        data.mc_4p0.shift();
        data.mc_10p0.shift();
        data.nc_0p5.shift();
        data.nc_1p0.shift();
        data.nc_2p5.shift();
        data.nc_4p0.shift();
        data.nc_10p0.shift();
        data.typical_particle_size.shift();
    }

    updateCharts();
    updateTable(reading);
}

function updateCharts() {
    massChart.data.labels = data.timestamps;
    massChart.data.datasets[0].data = data.mc_1p0;
    massChart.data.datasets[1].data = data.mc_2p5;
    massChart.data.datasets[2].data = data.mc_4p0;
    massChart.data.datasets[3].data = data.mc_10p0;
    massChart.update();

    numberChart.data.labels = data.timestamps;
    numberChart.data.datasets[0].data = data.nc_0p5;
    numberChart.data.datasets[1].data = data.nc_1p0;
    numberChart.data.datasets[2].data = data.nc_2p5;
    numberChart.data.datasets[3].data = data.nc_4p0;
    numberChart.data.datasets[4].data = data.nc_10p0;
    numberChart.update();
}

function populateTable() {
    const tbody = document.getElementById('table-body');
    tbody.innerHTML = '';

    const parameters = [
        { key: 'mc_1p0', label: 'Mass Concentration PM1.0', unit: 'µg/m³' },
        { key: 'mc_2p5', label: 'Mass Concentration PM2.5', unit: 'µg/m³' },
        { key: 'mc_4p0', label: 'Mass Concentration PM4.0', unit: 'µg/m³' },
        { key: 'mc_10p0', label: 'Mass Concentration PM10.0', unit: 'µg/m³' },
        { key: 'nc_0p5', label: 'Number Concentration PM0.5', unit: '#/cm³' },
        { key: 'nc_1p0', label: 'Number Concentration PM1.0', unit: '#/cm³' },
        { key: 'nc_2p5', label: 'Number Concentration PM2.5', unit: '#/cm³' },
        { key: 'nc_4p0', label: 'Number Concentration PM4.0', unit: '#/cm³' },
        { key: 'nc_10p0', label: 'Number Concentration PM10.0', unit: '#/cm³' },
        { key: 'typical_particle_size', label: 'Typical Particle Size', unit: 'µm' }
    ];

    parameters.forEach(param => {
        const row = document.createElement('tr');
        row.innerHTML = `
            <td>${param.label}</td>
            <td id="value-${param.key}">--</td>
            <td>${param.unit}</td>
        `;
        tbody.appendChild(row);
    });
}

function updateTable(reading) {
    const parameters = [
        'mc_1p0', 'mc_2p5', 'mc_4p0', 'mc_10p0',
        'nc_0p5', 'nc_1p0', 'nc_2p5', 'nc_4p0', 'nc_10p0',
        'typical_particle_size'
    ];

    parameters.forEach(param => {
        const elem = document.getElementById(`value-${param}`);
        if (elem && reading[param] !== undefined) {
            elem.textContent = reading[param].toFixed(2);
        }
    });
}

function updateConnectionStatus(connected) {
    const statusDot = document.getElementById('status-indicator');
    const statusText = document.getElementById('status-text');

    if (connected) {
        statusDot.classList.remove('disconnected');
        statusDot.classList.add('connected');
        statusText.textContent = 'Connected';
    } else {
        statusDot.classList.remove('connected');
        statusDot.classList.add('disconnected');
        statusText.textContent = 'Disconnected';
    }
}