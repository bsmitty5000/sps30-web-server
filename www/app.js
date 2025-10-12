document.addEventListener('DOMContentLoaded', () => {
    const statusDiv = document.getElementById('status');
    const dataContainer = document.getElementById('data-container');
    const registerBtn = document.getElementById('registerBtn');
    const disconnectBtn = document.getElementById('disconnectBtn');
    let ws;

    function connect() {
        const proto = window.location.protocol === 'https:' ? 'wss' : 'ws';
        const host = window.location.host;
        const wsUri = `${proto}://${host}/ws`;

        console.log(`Connecting to ${wsUri}`);
        ws = new WebSocket(wsUri);

        ws.onopen = () => {
            console.log('WebSocket connection opened');
            statusDiv.textContent = 'Status: Connected (Click Register)';
            statusDiv.style.color = '#17a2b8'; // Info color
            registerBtn.disabled = false;
            disconnectBtn.disabled = false;
            registerBtn.textContent = 'Register for Broadcasts';
        };

        ws.onmessage = (event) => {
            console.log('Message from server:', event.data);
            try {
                const jsonData = JSON.parse(event.data);

                // --- NEW: Handle ACK/NACK responses from the server ---
                if (jsonData.response_for) {
                    handleServerResponse(jsonData);
                } 
                // Handle broadcast data
                else if (jsonData.hasOwnProperty('uptime')) {
                    document.getElementById('random-number').textContent = jsonData.randomNumber;
                    document.getElementById('uptime').textContent = jsonData.uptime;
                    document.getElementById('status').textContent = jsonData.status;
                }

            } catch (e) {
                console.error('Error parsing JSON:', e);
                dataContainer.innerHTML = `<pre>${event.data}</pre>`;
            }
        };

        ws.onclose = () => {
            console.log('WebSocket connection closed');
            statusDiv.textContent = 'Status: Disconnected. Retrying in 3s...';
            statusDiv.style.color = '#dc3545';
            registerBtn.disabled = true;
            disconnectBtn.disabled = true;
            setTimeout(connect, 3000);
        };

        ws.onerror = (error) => {
            console.error('WebSocket error:', error);
            statusDiv.textContent = 'Status: Connection Error';
            statusDiv.style.color = '#dc3545';
        };
    }
    
    function handleServerResponse(response) {
        if (response.response_for === 'registerClient') {
            if (response.status === 'success') {
                console.log('Registration successful!');
                registerBtn.disabled = true;
                registerBtn.textContent = 'Registered';
                statusDiv.textContent = 'Status: Registered and Receiving Data';
                statusDiv.style.color = '#28a745';
            } else {
                console.error('Registration failed:', response.message);
                // Use a more user-friendly notification in a real app
                alert(`Registration Failed: ${response.message}`); 
                registerBtn.disabled = false; // Allow user to try again
            }
        }
    }

    registerBtn.addEventListener('click', () => {
        if (ws && ws.readyState === WebSocket.OPEN) {
            const message = { action: "registerClient" };
            ws.send(JSON.stringify(message));
            console.log('Sent "registerClient" message.');
        }
    });

    disconnectBtn.addEventListener('click', () => {
        if (ws && ws.readyState === WebSocket.OPEN) {
            const message = { action: "closeConnection" };
            ws.send(JSON.stringify(message));
            console.log('Sent "closeConnection" message.');
        }
    });

    connect();
});
