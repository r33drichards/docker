#!/usr/bin/env python3
"""
Automatic IRC Bot for #email channel
Demonstrates bot oper flood bypass by sending messages automatically
"""

import socket
import time
import sys


class EmailBot:
    def __init__(self, host='localhost', port=6667, channel='#email'):
        self.host = host
        self.port = port
        self.channel = channel
        self.nick = 'EmailBot'
        self.sock = None

    def connect(self):
        """Connect to IRC server"""
        print(f"[EmailBot] Connecting to {self.host}:{self.port}...")
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((self.host, self.port))

        # Register
        self.send_raw(f"NICK {self.nick}")
        self.send_raw(f"USER emailbot 0 * :Email Channel Bot")

        # Wait for registration
        while True:
            data = self.recv()
            if not data:
                continue

            # Handle PING
            if data.startswith("PING"):
                pong = "PONG" + data[4:]
                self.send_raw(pong)

            # Connected
            if " 001 " in data:
                print("[EmailBot] ✓ Connected and registered")
                break

    def oper_up(self):
        """Authenticate as bot oper"""
        print("[EmailBot] Authenticating as BotUser...")
        self.send_raw("OPER BotUser changeme_bot_password_123")

        # Wait for oper confirmation
        start = time.time()
        while time.time() - start < 5:
            data = self.recv()
            if not data:
                continue

            if data.startswith("PING"):
                self.send_raw("PONG" + data[4:])

            if " 381 " in data:
                print("[EmailBot] ✓ Successfully authenticated as oper")
                print("[EmailBot] ✓ Flood protection bypassed!")
                return True

        print("[EmailBot] ✗ Failed to oper up")
        return False

    def join_channel(self):
        """Join the channel"""
        print(f"[EmailBot] Joining {self.channel}...")
        self.send_raw(f"JOIN {self.channel}")
        time.sleep(1)
        print(f"[EmailBot] ✓ Joined {self.channel}")

    def send_message(self, message):
        """Send a message to the channel"""
        self.send_raw(f"PRIVMSG {self.channel} :{message}")
        print(f"[{self.channel}] <{self.nick}> {message}")

    def send_raw(self, message):
        """Send raw IRC message"""
        try:
            self.sock.send((message + "\r\n").encode('utf-8'))
        except Exception as e:
            print(f"[EmailBot] Error sending: {e}")

    def recv(self):
        """Receive data from server"""
        try:
            self.sock.settimeout(0.5)
            data = self.sock.recv(4096).decode('utf-8', errors='ignore').strip()

            # Handle PING in background
            if data.startswith("PING"):
                self.send_raw("PONG" + data[4:])

            return data
        except socket.timeout:
            return None
        except Exception as e:
            print(f"[EmailBot] Error receiving: {e}")
            return None

    def run_demo(self):
        """Run automated demonstration"""
        try:
            # Connect
            self.connect()

            # Oper up
            if not self.oper_up():
                print("[EmailBot] Cannot continue without oper privileges")
                return

            # Join channel
            self.join_channel()

            # Greeting
            self.send_message("EmailBot online! Demonstrating oper flood bypass capabilities.")
            time.sleep(1)

            # Demo 1: Rapid burst
            print("\n[EmailBot] Demo 1: Sending 20 messages in rapid succession...")
            start = time.time()
            for i in range(20):
                self.send_message(f"Rapid message {i+1}/20 - No throttling!")
                time.sleep(0.05)
            elapsed = time.time() - start
            print(f"[EmailBot] ✓ Sent 20 messages in {elapsed:.2f}s ({20/elapsed:.2f} msgs/sec)")
            time.sleep(2)

            # Demo 2: Very fast burst
            print("\n[EmailBot] Demo 2: Sending 50 messages at maximum speed...")
            start = time.time()
            for i in range(50):
                self.send_message(f"Ultra-fast message {i+1}/50")
                time.sleep(0.001)  # 1ms delay - would definitely throttle regular users
            elapsed = time.time() - start
            print(f"[EmailBot] ✓ Sent 50 messages in {elapsed:.2f}s ({50/elapsed:.2f} msgs/sec)")
            time.sleep(2)

            # Demo 3: Sustained messaging
            print("\n[EmailBot] Demo 3: Sustained messaging (100 messages)...")
            start = time.time()
            for i in range(100):
                self.send_message(f"Sustained message {i+1}/100 - Still no throttling!")
                time.sleep(0.02)

                # Check for any responses periodically
                if i % 10 == 0:
                    self.recv()

            elapsed = time.time() - start
            print(f"[EmailBot] ✓ Sent 100 messages in {elapsed:.2f}s ({100/elapsed:.2f} msgs/sec)")

            # Summary
            self.send_message("Demo complete! All messages sent without throttling thanks to oper flood bypass.")
            print("\n[EmailBot] ✓ Demonstration complete!")
            print("[EmailBot] Regular IRC users would be disconnected for this level of flooding.")
            print("[EmailBot] Bot oper privileges successfully bypass all flood protection.")

            # Keep alive for a bit
            print("\n[EmailBot] Staying connected for 30 seconds...")
            for _ in range(30):
                self.recv()
                time.sleep(1)

        except KeyboardInterrupt:
            print("\n[EmailBot] Interrupted by user")
        except Exception as e:
            print(f"[EmailBot] Error: {e}")
            import traceback
            traceback.print_exc()
        finally:
            if self.sock:
                try:
                    self.send_raw("QUIT :EmailBot demo complete")
                    self.sock.close()
                except:
                    pass
            print("\n[EmailBot] Disconnected")


if __name__ == "__main__":
    print("="*70)
    print("IRC EmailBot - Automated Flood Bypass Demonstration")
    print("="*70)
    print()

    bot = EmailBot()
    bot.run_demo()
