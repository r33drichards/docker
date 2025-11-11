# Bot Oper Quick Start Guide

This guide will get you up and running with InspIRCd bot flood bypass in under 5 minutes.

## Quick Start (Using Docker Compose)

### 1. Start InspIRCd with Bot Config

```bash
# Navigate to the examples directory
cd examples

# Start InspIRCd with the bot configuration
docker-compose -f docker-compose-bot.yml up -d

# Check if it's running
docker-compose -f docker-compose-bot.yml logs
```

The `conf.d/bot-oper.conf` file will be automatically loaded by InspIRCd.

### 2. Test the Bot Configuration

```bash
# Run the test script
cd ../tests
./run_bot_test.sh

# Or directly with Python:
python3 test_bot_flood.py
```

### 3. Expected Results

The test will:
1. ✓ Connect as a regular user → Gets throttled/kicked when flooding
2. ✓ Connect as bot with oper → Sends all messages without throttling

If both tests pass, your bot configuration is working!

## Quick Start (Using Docker Run)

### 1. Start InspIRCd

```bash
docker run -d \
  --name inspircd-bot \
  -p 6667:6667 \
  -p 6697:6697 \
  -v $(pwd)/examples/conf.d:/inspircd/conf.d:ro \
  -e INSP_ENABLE_DNSBL=no \
  inspircd/inspircd-docker
```

### 2. Test It

```bash
cd tests
./run_bot_test.sh
```

## Using in Your Bot

### Python Example

```python
import socket
import time

# Connect to IRC
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(('localhost', 6667))

# Register
sock.send(b"NICK MyBot\r\n")
sock.send(b"USER mybot 0 * :My Bot\r\n")

# Wait for welcome message (001)
while True:
    data = sock.recv(4096).decode('utf-8')
    print(data)

    # Handle PING
    if data.startswith('PING'):
        sock.send(('PONG' + data[4:]).encode('utf-8'))

    # Connection registered
    if ' 001 ' in data:
        break

# Authenticate as bot oper
sock.send(b"OPER BotUser changeme_bot_password_123\r\n")

# Wait for oper confirmation (381)
while True:
    data = sock.recv(4096).decode('utf-8')
    print(data)

    if ' 381 ' in data:
        print("Successfully opered up!")
        break

# Now you can send messages rapidly without throttling!
sock.send(b"JOIN #test\r\n")
time.sleep(1)

for i in range(100):
    sock.send(f"PRIVMSG #test :Message {i}\r\n".encode('utf-8'))
    time.sleep(0.01)  # Very fast - would throttle regular users

print("Sent 100 messages without being throttled!")
```

### Node.js Example

```javascript
const net = require('net');

const client = new net.Socket();
client.connect(6667, 'localhost', () => {
    console.log('Connected');

    // Register
    client.write('NICK MyBot\r\n');
    client.write('USER mybot 0 * :My Bot\r\n');
});

client.on('data', (data) => {
    const message = data.toString();
    console.log(message);

    // Handle PING
    if (message.startsWith('PING')) {
        client.write('PONG' + message.substring(4));
    }

    // Connection registered - oper up
    if (message.includes(' 001 ')) {
        client.write('OPER BotUser changeme_bot_password_123\r\n');
    }

    // Successfully opered
    if (message.includes(' 381 ')) {
        console.log('Opered up! Sending messages...');

        client.write('JOIN #test\r\n');

        setTimeout(() => {
            for (let i = 0; i < 100; i++) {
                client.write(`PRIVMSG #test :Message ${i}\r\n`);
            }
            console.log('Sent 100 messages!');
        }, 1000);
    }
});
```

## Customizing the Configuration

### Change the Password

**IMPORTANT:** Change the default password!

1. Connect to your IRC server
2. Run: `/MKPASSWD bcrypt YourSecurePassword`
3. Copy the hash output
4. Edit `examples/conf.d/bot-oper.conf`:

```xml
<oper
      name="BotUser"
      hash="bcrypt"
      password="$2y$10$your_hash_here"
      host="*@*"
      type="BotType">
```

5. Restart InspIRCd:
```bash
docker-compose -f docker-compose-bot.yml restart
```

### Restrict to Specific IP

Edit `examples/conf.d/bot-oper.conf`:

```xml
<oper
      name="BotUser"
      password="..."
      host="*@192.168.1.100"  <!-- Only allow from this IP -->
      type="BotType">
```

### Require SSL

```xml
<oper
      name="BotUser"
      password="..."
      host="*@*"
      type="BotType"
      sslonly="yes">  <!-- Require SSL connection -->
```

## Troubleshooting

### Test Fails: "Could not oper up"

Check:
- Is the password correct?
- Is `bot-oper.conf` mounted and included?
- Check Docker logs: `docker-compose -f docker-compose-bot.yml logs`

### Bot Still Gets Throttled

Check:
- Did the bot receive `381 RPL_YOUREOPER` response?
- Are flood bypass privs set in the class? (`users/flood/*`)
- Try: `/REHASH` or restart InspIRCd

### Cannot Connect to Server

```bash
# Check if port is open
telnet localhost 6667

# Check Docker is running
docker ps

# Check Docker logs
docker-compose -f docker-compose-bot.yml logs -f
```

## What Privileges Does the Bot Have?

The bot oper account has:

✓ **Flood bypass privileges:**
- `users/flood/no-throttle` - No rate limiting
- `users/flood/no-fakelag` - No fake lag
- `users/flood/increased-buffers` - Larger buffers

✓ **Basic IRC commands:**
- PRIVMSG, NOTICE, JOIN, PART, KICK, MODE, etc.

✗ **No admin privileges:**
- Cannot KILL, KLINE, or ban users
- Cannot link/unlink servers
- Cannot load/unload modules
- Cannot see hidden user info

## Channel Restrictions

**Important:** Flood bypass privileges are **global** and cannot be restricted to specific channels. However:

- Your bot code can limit itself to specific channels
- Channel modes still apply (like +m moderated)
- The privileges apply server-wide once authenticated

## Security Notes

- Change the default password!
- Use hashed passwords (bcrypt)
- Restrict `host` field to bot's IP
- Consider requiring SSL
- Monitor bot behavior
- Don't give bot admin privileges

## More Information

- Full documentation: `../conf/BOT-OPER-README.md`
- Test script: `../tests/test_bot_flood.py`
- Configuration: `../conf/bot-oper.conf`
- InspIRCd docs: https://docs.inspircd.org

## Support

- InspIRCd Discord: https://discord.gg/inspircd
- GitHub Issues: https://github.com/inspircd/docker/issues
- IRC: #inspircd on Teranova (irc.teranova.net)
