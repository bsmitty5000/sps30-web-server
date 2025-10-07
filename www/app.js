// Configuration
const MAX_DATA_POINTS = 300; // 5 minutes at 1 reading/sec
const CANVAS_PADDING = 40;
const LINE_COLORS = {
    pm1: '#3b82f6',   // blue
    pm25: '#ef4444',  // red
    pm4: '#f59e0b',   // orange
    pm10: '#8b5cf6'   // purple
};

// Data storage
const dataHistory = {
    timestamps: [],
    pm1: [],
    pm25: [],
    pm4: [],
    pm10: []
};

// Chart state
let chartVisible = {
    pm1: true,
    pm25: true,
    pm4: true,
    pm10: true
};

// Connection state
let lastUpdateTime = null;
let updateCount = 0;
let rateCheckInterval = null;

// Get DOM elements
const canvas = document.getElementById('chart');
const ctx = canvas.getContext('2d');
const statusEl = document.getElementById('status');
const statusText = statusEl.querySelector('.status-text');
const statusDot = statusEl.querySelector('.status-dot');

// Metric value elements
const metricElements = {
    pm1: document.getElementById('pm1'),
    pm25: document.getElementById('pm25'),
    pm4: document.getElementById('pm4'),
    pm10: document.getElementById('pm10')
};

// Info elements
const lastUpdateEl = document.getElementById('last-update');
const updateRateEl = document.getElementById('update-rate');

// Checkbox listeners
document.getElementById('show-pm1').addEventListener('change', (e) => {
    chartVisible.pm1 = e.target.checked;
    drawChart();
});
document.getElementById('show-pm25').addEventListener('change', (e) => {
    chartVisible.pm25 = e.target.checked;
    drawChart();
});
document.getElementById('show-pm4').addEventListener('change', (e) => {
    chartVisible.pm4 = e.target.checked;
    drawChart();
});
document.getElementById('show-pm10').addEventListener('change', (e) => {
    chartVisible.pm10 = e.target.checked;
    drawChart();
});

// Set canvas size
function resizeCanvas() {
    const container = canvas.parentElement;
    canvas.width = container.clientWidth;
    canvas.height = 400;
    drawChart();
}

window.addEventListener('resize', resizeCanvas);
resizeCanvas();

// Update status indicator
function setStatus(connected) {
    if (connected) {
        statusText.textContent = 'Connected';
        statusDot.classList.add('connected');
    } else {
        statusText.textContent = 'Disconnected';
        statusDot.classList.remove('connected');
    }
}

// Add data point
function addDataPoint(data) {
    const now = Date.now();
    
    dataHistory.timestamps.push(now);
    dataHistory.pm1.push(data.pm1);
    dataHistory.pm25.push(data.pm25);
    dataHistory.pm4.push(data.pm4);
    dataHistory.pm10.push(data.pm10);
    
    // Keep only last MAX_DATA_POINTS
    if (dataHistory.timestamps.length > MAX_DATA_POINTS) {
        dataHistory.timestamps.shift();
        dataHistory.pm1.shift();
        dataHistory.pm25.shift();
        dataHistory.pm4.shift();
        dataHistory.pm10.shift();
    }
    
    // Update metric displays
    metricElements.pm1.textContent = data.pm1.toFixed(1);
    metricElements.pm25.textContent = data.pm25.toFixed(1);
    metricElements.pm4.textContent = data.pm4.toFixed(1);
    metricElements.pm10.textContent = data.pm10.toFixed(1);
    
    // Update last update time
    lastUpdateTime = now;
    lastUpdateEl.textContent = new Date(now).toLocaleTimeString();
    
    // Track update rate
    updateCount++;
    
    drawChart();
}

// Calculate update rate
function calculateUpdateRate() {
    if (updateCount > 0) {
        const rate = updateCount;
        updateRateEl.textContent = `${rate} updates/sec`;
        updateCount = 0;
    }
}

// Draw chart
function drawChart() {
    const width = canvas.width;
    const height = canvas.height;
    
    // Clear canvas
    ctx.clearRect(0, 0, width, height);
    
    if (dataHistory.timestamps.length < 2) {
        ctx.fillStyle = '#6b7280';
        ctx.font = '14px sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText('Waiting for data...', width / 2, height / 2);
        return;
    }
    
    // Calculate drawing area
    const plotWidth = width - CANVAS_PADDING * 2;
    const plotHeight = height - CANVAS_PADDING * 2;
    const plotX = CANVAS_PADDING;
    const plotY = CANVAS_PADDING;
    
    // Find max value for scaling
    let maxValue = 10;
    const allValues = [];
    if (chartVisible.pm1) allValues.push(...dataHistory.pm1);
    if (chartVisible.pm25) allValues.push(...dataHistory.pm25);
    if (chartVisible.pm4) allValues.push(...dataHistory.pm4);
    if (chartVisible.pm10) allValues.push(...dataHistory.pm10);
    
    if (allValues.length > 0) {
        maxValue = Math.max(...allValues, 10);
        maxValue = Math.ceil(maxValue * 1.1); // Add 10% headroom
    }
    
    // Draw grid
    ctx.strokeStyle = '#e5e7eb';
    ctx.lineWidth = 1;
    
    // Horizontal grid lines
    for (let i = 0; i <= 5; i++) {
        const y = plotY + (plotHeight / 5) * i;
        ctx.beginPath();
        ctx.moveTo(plotX, y);
        ctx.lineTo(plotX + plotWidth, y);
        ctx.stroke();
        
        // Y-axis labels
        const value = maxValue - (maxValue / 5) * i;
        ctx.fillStyle = '#6b7280';
        ctx.font = '12px sans-serif';
        ctx.textAlign = 'right';
        ctx.fillText(value.toFixed(0), plotX - 5, y + 4);
    }
    
    // Draw data lines
    const drawLine = (data, color, visible) => {
        if (!visible || data.length < 2) return;
        
        ctx.strokeStyle = color;
        ctx.lineWidth = 2;
        ctx.beginPath();
        
        for (let i = 0; i < data.length; i++) {
            const x = plotX + (plotWidth / (MAX_DATA_POINTS - 1)) * i;
            const y = plotY + plotHeight - (data[i] / maxValue) * plotHeight;
            
            if (i === 0) {
                ctx.moveTo(x, y);
            } else {
                ctx.lineTo(x, y);
            }
        }
        
        ctx.stroke();
    };
    
    drawLine(dataHistory.pm1, LINE_COLORS.pm1, chartVisible.pm1);
    drawLine(dataHistory.pm25, LINE_COLORS.pm25, chartVisible.pm25);
    drawLine(dataHistory.pm4, LINE_COLORS.pm4, chartVisible.pm4);
    drawLine(dataHistory.pm10, LINE_COLORS.pm10, chartVisible.pm10);
    
    // Draw axes
    ctx.strokeStyle = '#374151';
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(plotX, plotY);
    ctx.lineTo(plotX, plotY + plotHeight);
    ctx.lineTo(plotX + plotWidth, plotY + plotHeight);
    ctx.stroke();
    
    // Axis labels
    ctx.fillStyle = '#374151';
    ctx.font = '14px sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('µg/m³', plotX - 20, plotY - 10);
}

// Connect to ESP32 via WebSocket
let ws = null;
let reconnectTimeout = null;

function connectWebSocket() {
    // Determine WebSocket URL based on current location
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${protocol}//${window.location.host}/ws`;
    
    console.log('Connecting to WebSocket:', wsUrl);
    
    try {
        ws = new WebSocket(wsUrl);
        
        ws.onopen = () => {
            console.log('WebSocket connected');
            setStatus(true);
            
            // Clear any pending reconnection attempts
            if (reconnectTimeout) {
                clearTimeout(reconnectTimeout);
                reconnectTimeout = null;
            }
            
            // Optional: Send initial message to server
            ws.send(JSON.stringify({ type: 'hello', message: 'Client connected' }));
        };
        
        ws.onmessage = (event) => {
            try {
                const data = JSON.parse(event.data);
                
                // Handle different message types if needed
                if (data.type === 'sensor_data') {
                    addDataPoint(data);
                } else if (data.pm1 !== undefined) {
                    // Direct sensor data format
                    addDataPoint(data);
                } else {
                    console.log('Unknown message type:', data);
                }
            } catch (e) {
                console.error('Failed to parse WebSocket message:', e, event.data);
            }
        };
        
        ws.onerror = (error) => {
            console.error('WebSocket error:', error);
            setStatus(false);
        };
        
        ws.onclose = (event) => {
            console.log('WebSocket closed:', event.code, event.reason);
            setStatus(false);
            ws = null;
            
            // Attempt to reconnect after 3 seconds
            reconnectTimeout = setTimeout(() => {
                console.log('Attempting to reconnect...');
                connectWebSocket();
            }, 3000);
        };
        
    } catch (error) {
        console.error('Failed to create WebSocket:', error);
        setStatus(false);
        
        // Retry connection
        reconnectTimeout = setTimeout(connectWebSocket, 3000);
    }
}

// Function to send commands to ESP32 (optional)
function sendCommand(command, data = {}) {
    if (ws && ws.readyState === WebSocket.OPEN) {
        const message = {
            type: command,
            ...data
        };
        ws.send(JSON.stringify(message));
        console.log('Sent command:', message);
    } else {
        console.warn('WebSocket not connected, cannot send command');
    }
}

// Generate mock data for testing (remove when connecting to real ESP32)
function generateMockData() {
    const baseValues = {
        pm1: 10,
        pm25: 15,
        pm4: 20,
        pm10: 25
    };
    
    setInterval(() => {
        const data = {
            pm1: Math.max(0, baseValues.pm1 + (Math.random() - 0.5) * 5),
            pm25: Math.max(0, baseValues.pm25 + (Math.random() - 0.5) * 8),
            pm4: Math.max(0, baseValues.pm4 + (Math.random() - 0.5) * 10),
            pm10: Math.max(0, baseValues.pm10 + (Math.random() - 0.5) * 12)
        };
        addDataPoint(data);
        setStatus(true);
    }, 100); // Update every 100ms for demo
}

// Initialize
rateCheckInterval = setInterval(calculateUpdateRate, 1000);

// Clean up on page unload
window.addEventListener('beforeunload', () => {
    if (ws) {
        ws.close();
    }
    if (reconnectTimeout) {
        clearTimeout(reconnectTimeout);
    }
});

// IMPORTANT: Comment out generateMockData() and uncomment connectWebSocket()
// when connecting to your ESP32

generateMockData(); // FOR TESTING ONLY - Remove this line for production
// connectWebSocket(); // Uncomment this for real ESP32 connection