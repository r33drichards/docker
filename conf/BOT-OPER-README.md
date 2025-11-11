# Bot Oper Configuration Guide

This guide explains how to configure InspIRCd to allow IRC bots to bypass flood protection using oper privileges.

## Overview

The `bot-oper.conf` file provides a special oper configuration that grants flood bypass privileges to IRC bots. This allows bots to send messages rapidly without being throttled or disconnected by flood protection.

## Can I Restrict Privileges to a Single Channel?

**No.** InspIRCd's oper privileges are **global** and apply to the entire server connection, not to specific channels. The flood bypass privileges (`users/flood/no-throttle`, `users/flood/no-fakelag`, `users/flood/increased-buffers`) will work on all channels once the bot authenticates.

However:
- Your bot application can be coded to only interact with specific channels
- You can use channel modes to restrict actions in specific channels
- The flood bypass itself cannot be limited to certain channels

## Setup Instructions

### 1. Include the Bot Configuration

Add the following line to your `inspircd.conf`:

```xml
<include file="bot-oper.conf">
```

Or, if using Docker, mount the config file and include it.

### 2. Customize the Configuration

Edit `conf/bot-oper.conf` and change:

#### a) Password (IMPORTANT!)

Generate a secure hashed password:

```bash
# Connect to your IRC server and run:
/MKPASSWD bcrypt YourSecurePasswordHere
```

Then update the oper block in `bot-oper.conf`:

```xml
<oper
      name="BotUser"
      hash="bcrypt"
      password="$2y$10$your_generated_hash_here"
      host="*@*"
      type="BotType">
```

#### b) Host Restriction (Recommended for Security!)

Restrict which hosts can use this oper account. Options:

```xml
<!-- Allow only localhost -->
host="*@127.0.0.1 *@::1"

<!-- Allow specific subnet -->
host="*@192.168.1.0/24"

<!-- Allow specific hostname -->
host="botuser@bot.example.com"

<!-- Allow any host (NOT RECOMMENDED for production) -->
host="*@*"
```

#### c) Virtual Host

Change the vhost to match your server:

```xml
<type
    name="BotType"
    classes="BotUser"
    vhost="bot.yourserver.com"
    maxchans="128">
```

#### d) SSL Requirement

For better security, require SSL connections:

```xml
<oper
      name="BotUser"
      ...
      sslonly="yes">
```

### 3. Restart/Rehash Server

After making changes:

```bash
# If using Docker:
docker restart inspircd

# Or if InspIRCd is running:
/REHASH
```

## Using the Bot Oper Account

### From Your Bot Application

1. Connect to IRC server normally
2. Send NICK and USER commands
3. After successful registration, authenticate:

```
/OPER BotUser your_password_here
```

4. Wait for response `381 RPL_YOUREOPER`
5. Bot now has flood bypass privileges

### Example in Python (using socket)

```python
import socket

sock = socket.socket()
sock.connect(('irc.example.com', 6667))

# Register
sock.send(b"NICK MyBot\r\n")
sock.send(b"USER mybot 0 * :My Bot\r\n")

# Wait for 001 RPL_WELCOME
# ... (receive and handle messages) ...

# Oper up
sock.send(b"OPER BotUser your_password\r\n")

# Wait for 381 RPL_YOUREOPER
# ... (receive and handle messages) ...

# Now bot can send messages rapidly without throttling
sock.send(b"JOIN #channel\r\n")
sock.send(b"PRIVMSG #channel :Message 1\r\n")
sock.send(b"PRIVMSG #channel :Message 2\r\n")
# ... etc
```

## Testing the Configuration

Use the included test script to verify the flood bypass works:

```bash
# Basic test
python3 tests/test_bot_flood.py

# Custom server/port
python3 tests/test_bot_flood.py --host irc.example.com --port 6667

# Custom password
python3 tests/test_bot_flood.py --bot-password YourBotPassword

# Send more messages
python3 tests/test_bot_flood.py --messages 200

# Faster flooding
python3 tests/test_bot_flood.py --messages 100 --delay 0.001

# Skip regular user test
python3 tests/test_bot_flood.py --skip-regular
```

The test script will:
1. Connect as a regular user and flood a channel (should be throttled/kicked)
2. Connect as the bot, oper up, and flood the channel (should work without throttling)

### Expected Results

**Regular User Test:**
- Should be kicked or disconnected
- Error messages about flooding/throttling

**Bot User Test:**
- Should successfully send all messages
- No kicks or disconnections
- No flood errors

## Security Considerations

### Privileges Granted

The bot oper account has these privileges:

- `users/flood/no-throttle` - No command rate limiting
- `users/flood/no-fakelag` - No fake lag penalties
- `users/flood/increased-buffers` - Larger send/receive buffers

**WARNING:** These privileges allow unlimited message rate. The bot can consume significant CPU/bandwidth if misused.

### Best Practices

1. **Use Hashed Passwords** - Never use plaintext passwords in production
2. **Restrict Host Access** - Limit `host` field to bot's actual IP/hostname
3. **Require SSL** - Set `sslonly="yes"` if your bot supports SSL
4. **Minimal Commands** - The bot class only has basic IRC commands, no admin commands
5. **Monitor Bot Behavior** - Watch for abuse or bugs that could cause actual floods
6. **Separate from Admin Opers** - Don't give bot accounts admin privileges
7. **Keep Password Secret** - Don't share the bot oper password

### What the Bot Cannot Do

The bot oper account is intentionally limited:

- ✗ Cannot execute admin commands (KILL, KLINE, etc.)
- ✗ Cannot see auspex (hidden user info, IP addresses, etc.)
- ✗ Cannot link/unlink servers
- ✗ Cannot load/unload modules
- ✗ Only has basic IRC commands (JOIN, PART, PRIVMSG, etc.)

The bot can only:

- ✓ Send messages without throttling
- ✓ Join channels without limits (up to maxchans)
- ✓ Execute basic IRC commands

## Troubleshooting

### Bot Cannot Oper Up

1. Check password is correct
2. Verify `host` field allows bot's connection
3. Check if `sslonly="yes"` requires SSL but bot isn't using SSL
4. Look for errors in InspIRCd logs
5. Verify config file is included in `inspircd.conf`

### Bot Still Gets Throttled

1. Verify bot successfully opered up (check for 381 response)
2. Check InspIRCd logs for oper authentication
3. Ensure `users/flood/*` privileges are set in the class
4. Try rehashing server after config changes

### Connection Issues in Tests

1. Check server is running: `docker ps` or `netstat -an | grep 6667`
2. Verify port is accessible: `telnet localhost 6667`
3. Check firewall rules
4. Review InspIRCd logs for connection errors

## Additional Information

### Flood Protection Modules

InspIRCd has several flood protection mechanisms:

- **Command Throttling** - Limits commands per second (bypassed by `no-throttle`)
- **Fake Lag** - Adds artificial delay when flooding (bypassed by `no-fakelag`)
- **Send/Receive Queues** - Limited buffer sizes (bypassed by `increased-buffers`)

The bot oper privileges bypass all three.

### Alternative Approaches

Instead of oper privileges, you could:

1. **Increase Flood Limits** - Adjust `<connect>` block threshold/commandrate
2. **Custom Connect Class** - Create a connect class with higher limits for bot's IP
3. **Services Integration** - Use IRC services (Anope, Atheme) for certain bot functions

However, oper privileges are the most powerful and flexible approach for bots that need to send many messages rapidly.

## Support

For issues with:
- InspIRCd configuration: https://docs.inspircd.org
- This Docker image: https://github.com/inspircd/docker
- Bot development: Your bot framework's documentation

## License

This configuration is part of the InspIRCd Docker project and follows the same license.
