#!/usr/bin/env python3
"""
IRC Bot Flood Test Script

This script tests the bot oper flood bypass configuration by:
1. Connecting as a regular user and attempting to flood (should be throttled/kicked)
2. Connecting as a bot with oper privileges and flooding (should work)

Usage:
    python3 test_bot_flood.py [options]

Options:
    --host HOST          IRC server hostname (default: localhost)
    --port PORT          IRC server port (default: 6667)
    --bot-password PASS  Bot oper password (default: changeme_bot_password_123)
    --messages N         Number of messages to send (default: 100)
    --delay SECONDS      Delay between messages in seconds (default: 0.01)
"""

import socket
import time
import sys
import argparse
from typing import Optional


class IRCClient:
    """Simple IRC client for testing flood protection"""

    def __init__(self, host: str, port: int, nick: str, user: str, realname: str):
        self.host = host
        self.port = port
        self.nick = nick
        self.user = user
        self.realname = realname
        self.sock: Optional[socket.socket] = None
        self.connected = False

    def connect(self) -> bool:
        """Connect to IRC server"""
        try:
            print(f"[{self.nick}] Connecting to {self.host}:{self.port}...")
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(10)
            self.sock.connect((self.host, self.port))

            # Send NICK and USER
            self.send_raw(f"NICK {self.nick}")
            self.send_raw(f"USER {self.user} 0 * :{self.realname}")

            # Wait for connection to be established (001 RPL_WELCOME)
            start_time = time.time()
            while time.time() - start_time < 10:
                data = self.recv()
                if data:
                    print(f"[{self.nick}] << {data}")

                    # Respond to PING
                    if data.startswith("PING"):
                        pong = "PONG" + data[4:]
                        self.send_raw(pong)

                    # Check for successful registration
                    if " 001 " in data:
                        print(f"[{self.nick}] ✓ Successfully connected and registered")
                        self.connected = True
                        return True

                    # Check for errors
                    if "ERROR" in data or "banned" in data.lower():
                        print(f"[{self.nick}] ✗ Connection error: {data}")
                        return False

            print(f"[{self.nick}] ✗ Timeout waiting for connection")
            return False

        except Exception as e:
            print(f"[{self.nick}] ✗ Connection failed: {e}")
            return False

    def send_raw(self, message: str):
        """Send raw IRC message"""
        if not self.sock:
            return
        try:
            self.sock.send((message + "\r\n").encode('utf-8'))
        except Exception as e:
            print(f"[{self.nick}] ✗ Send error: {e}")

    def recv(self) -> Optional[str]:
        """Receive data from server"""
        if not self.sock:
            return None
        try:
            data = self.sock.recv(4096).decode('utf-8', errors='ignore').strip()
            return data
        except socket.timeout:
            return None
        except Exception as e:
            print(f"[{self.nick}] ✗ Receive error: {e}")
            return None

    def oper(self, username: str, password: str) -> bool:
        """Authenticate as operator"""
        print(f"[{self.nick}] Attempting to OPER as {username}...")
        self.send_raw(f"OPER {username} {password}")

        # Wait for response
        start_time = time.time()
        while time.time() - start_time < 5:
            data = self.recv()
            if data:
                print(f"[{self.nick}] << {data}")

                # Respond to PING
                if data.startswith("PING"):
                    pong = "PONG" + data[4:]
                    self.send_raw(pong)

                # Check for successful oper
                if " 381 " in data:  # RPL_YOUREOPER
                    print(f"[{self.nick}] ✓ Successfully opered up")
                    return True

                # Check for oper failure
                if " 464 " in data or " 491 " in data:  # ERR_PASSWDMISMATCH or ERR_NOOPERHOST
                    print(f"[{self.nick}] ✗ Oper failed: {data}")
                    return False

        print(f"[{self.nick}] ✗ Oper timeout")
        return False

    def join(self, channel: str) -> bool:
        """Join a channel"""
        print(f"[{self.nick}] Joining {channel}...")
        self.send_raw(f"JOIN {channel}")

        # Wait for join confirmation
        start_time = time.time()
        while time.time() - start_time < 5:
            data = self.recv()
            if data:
                if "PING" in data:
                    self.send_raw("PONG" + data[4:])
                if "JOIN" in data and channel in data:
                    print(f"[{self.nick}] ✓ Joined {channel}")
                    return True
        return False

    def flood_test(self, channel: str, num_messages: int, delay: float = 0.01) -> dict:
        """
        Send many messages rapidly to test flood protection

        Returns:
            dict with test results including messages sent, errors, etc.
        """
        results = {
            'messages_sent': 0,
            'messages_attempted': num_messages,
            'errors': [],
            'kicked': False,
            'disconnected': False,
        }

        print(f"\n[{self.nick}] Starting flood test: {num_messages} messages to {channel}")
        print(f"[{self.nick}] Delay between messages: {delay}s")

        start_time = time.time()

        for i in range(num_messages):
            message = f"PRIVMSG {channel} :Test message {i+1}/{num_messages}"
            self.send_raw(message)
            results['messages_sent'] += 1

            # Check for responses (errors, kicks, etc.)
            self.sock.settimeout(0.001)  # Very short timeout for checking
            try:
                data = self.recv()
                if data:
                    # Respond to PING immediately
                    if data.startswith("PING"):
                        self.send_raw("PONG" + data[4:])

                    # Check for errors
                    if "ERROR" in data:
                        results['errors'].append(data)
                        results['disconnected'] = True
                        print(f"[{self.nick}] ✗ DISCONNECTED: {data}")
                        break

                    if "KICK" in data:
                        results['kicked'] = True
                        print(f"[{self.nick}] ✗ KICKED: {data}")
                        break

                    # Check for flood-related errors
                    if any(x in data for x in ["flood", "Excess", "throttle", "RecvQ"]):
                        results['errors'].append(data)
                        print(f"[{self.nick}] ⚠ {data}")
            except:
                pass

            self.sock.settimeout(10)  # Reset timeout

            # Progress indicator
            if (i + 1) % 10 == 0:
                print(f"[{self.nick}] Sent {i+1}/{num_messages} messages...")

            time.sleep(delay)

        elapsed = time.time() - start_time
        results['elapsed_time'] = elapsed
        results['rate'] = results['messages_sent'] / elapsed if elapsed > 0 else 0

        print(f"\n[{self.nick}] Flood test complete:")
        print(f"  Messages sent: {results['messages_sent']}/{results['messages_attempted']}")
        print(f"  Time elapsed: {elapsed:.2f}s")
        print(f"  Rate: {results['rate']:.2f} msgs/sec")
        print(f"  Kicked: {results['kicked']}")
        print(f"  Disconnected: {results['disconnected']}")
        print(f"  Errors: {len(results['errors'])}")

        return results

    def disconnect(self):
        """Disconnect from server"""
        if self.sock:
            try:
                self.send_raw("QUIT :Test complete")
                self.sock.close()
            except:
                pass
            print(f"[{self.nick}] Disconnected")


def main():
    parser = argparse.ArgumentParser(description='Test IRC bot flood bypass configuration')
    parser.add_argument('--host', default='localhost', help='IRC server hostname')
    parser.add_argument('--port', type=int, default=6667, help='IRC server port')
    parser.add_argument('--bot-password', default='changeme_bot_password_123',
                       help='Bot oper password')
    parser.add_argument('--messages', type=int, default=100,
                       help='Number of messages to send in flood test')
    parser.add_argument('--delay', type=float, default=0.01,
                       help='Delay between messages in seconds')
    parser.add_argument('--skip-regular', action='store_true',
                       help='Skip the regular user test (only test bot)')

    args = parser.parse_args()

    test_channel = "#floodtest"

    print("=" * 70)
    print("IRC BOT FLOOD TEST")
    print("=" * 70)
    print(f"Server: {args.host}:{args.port}")
    print(f"Test channel: {test_channel}")
    print(f"Messages to send: {args.messages}")
    print(f"Delay: {args.delay}s")
    print("=" * 70)

    # Test 1: Regular user (should be throttled/kicked)
    if not args.skip_regular:
        print("\n\n### TEST 1: Regular User (Should be throttled) ###\n")
        regular = IRCClient(args.host, args.port, "FloodTest", "floodtest", "Flood Test User")

        if regular.connect():
            time.sleep(1)
            regular.join(test_channel)
            time.sleep(1)

            regular_results = regular.flood_test(test_channel, args.messages, args.delay)

            time.sleep(2)
            regular.disconnect()

            print("\nRegular user results:")
            if regular_results['kicked'] or regular_results['disconnected'] or regular_results['errors']:
                print("✓ PASS: Regular user was throttled/kicked as expected")
            else:
                print("⚠ WARNING: Regular user was NOT throttled (might indicate flood protection is disabled)")
        else:
            print("✗ FAIL: Could not connect as regular user")

        print("\n" + "=" * 70)
        time.sleep(3)

    # Test 2: Bot user with oper privileges (should NOT be throttled)
    print("\n\n### TEST 2: Bot with Oper Privileges (Should bypass flood protection) ###\n")
    bot = IRCClient(args.host, args.port, "FloodBot", "floodbot", "Flood Test Bot")

    if bot.connect():
        time.sleep(1)

        # Oper up
        if bot.oper("BotUser", args.bot_password):
            time.sleep(1)
            bot.join(test_channel)
            time.sleep(1)

            bot_results = bot.flood_test(test_channel, args.messages, args.delay)

            time.sleep(2)
            bot.disconnect()

            print("\nBot user results:")
            if not bot_results['kicked'] and not bot_results['disconnected'] and len(bot_results['errors']) == 0:
                print("✓ PASS: Bot successfully bypassed flood protection")
            else:
                print("✗ FAIL: Bot was throttled/kicked despite oper privileges")
        else:
            print("✗ FAIL: Could not oper up as bot")
            bot.disconnect()
    else:
        print("✗ FAIL: Could not connect as bot")

    print("\n" + "=" * 70)
    print("TEST COMPLETE")
    print("=" * 70)


if __name__ == "__main__":
    main()
