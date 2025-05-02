const WebSocket = require('ws');
const net = require('net');

const wss = new WebSocket.Server({ port: 8080 });
console.log("WebSocket proxy listening on ws://localhost:8080");

wss.on('connection', ws => {
    const tcpClient = new net.Socket();
    tcpClient.connect(12345, '127.0.0.1');

    let buffer = '';

    tcpClient.on('data', data => {
        buffer += data.toString();
        
        // Split by newlines to find complete messages
        let lines = buffer.split('\n');
        
        // Keep last partial line in the buffer
        buffer = lines.pop();

        for (const line of lines) {
            ws.send(line.trim()); // send one line at a time
        }
    });

    ws.on('message', msg => {
        tcpClient.write(msg + '\n');
    });

    ws.on('close', () => tcpClient.destroy());
    tcpClient.on('close', () => ws.close());
});
