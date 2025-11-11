#!/usr/bin/env python3
"""
Simple IRC Bot for #email channel
Demonstrates bot oper flood bypass in action
"""

import socket
import time
import threading


class EmailBot:
    def __init__(self, host='localhost', port=6667, channel='#email'):
        self.host = host
        self.port = port
        self.channel = channel
        self.nick = 'EmailBot'
        self.sock = None
        self.running = False

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

            print(f"[EmailBot] << {data[:100]}...")

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

    def send_raw(self, message):
        """Send raw IRC message"""
        try:
            self.sock.send((message + "\r\n").encode('utf-8'))
        except Exception as e:
            print(f"[EmailBot] Error sending: {e}")

    def recv(self):
        """Receive data from server"""
        try:
            self.sock.settimeout(0.1)
            data = self.sock.recv(4096).decode('utf-8', errors='ignore').strip()
            return data
        except socket.timeout:
            return None
        except Exception as e:
            print(f"[EmailBot] Error receiving: {e}")
            return None

    def handle_messages(self):
        """Handle incoming messages in background"""
        while self.running:
            data = self.recv()
            if not data:
                continue

            # Handle PING
            if data.startswith("PING"):
                self.send_raw("PONG" + data[4:])
                continue

            # Show channel messages
            if "PRIVMSG" in data and self.channel in data:
                # Parse: :nick!user@host PRIVMSG #channel :message
                try:
                    parts = data.split(" ", 3)
                    sender = parts[0].split("!")[0][1:]  # Remove leading :
                    message = parts[3][1:]  # Remove leading :
                    print(f"[{self.channel}] <{sender}> {message}")
                except:
                    pass

    def run(self):
        """Main bot loop"""
        try:
            # Connect
            self.connect()

            # Oper up
            if not self.oper_up():
                print("[EmailBot] Cannot continue without oper privileges")
                return

            # Join channel
            self.join_channel()

            # Start message handler in background
            self.running = True
            handler = threading.Thread(target=self.handle_messages, daemon=True)
            handler.start()

            # Send greeting
            self.send_message("EmailBot online! Oper privileges active. Flood protection bypassed.")
            time.sleep(0.5)

            # Demonstrate rapid messaging capability
            print("\n[EmailBot] Demonstrating flood bypass: Sending 10 messages rapidly...")
            for i in range(10):
                self.send_message(f"Rapid message test {i+1}/10")
                time.sleep(0.05)  # Very fast - would throttle regular users

            print("[EmailBot] ✓ All messages sent without throttling")

            # Interactive mode
            print("\n" + "="*70)
            print("Bot is now running in #email")
            print("Commands:")
            print("  Type a message to send to the channel")
            print("  Type 'flood <N>' to send N messages rapidly")
            print("  Type 'quit' to exit")
            print("="*70 + "\n")

            while self.running:
                try:
                    user_input = input("> ")

                    if not user_input:
                        continue

                    if user_input.lower() == 'quit':
                        break

                    if user_input.lower().startswith('flood '):
                        try:
                            count = int(user_input.split()[1])
                            print(f"[EmailBot] Flooding {count} messages...")
                            start = time.time()
                            for i in range(count):
                                self.send_message(f"Flood test message {i+1}/{count}")
                            elapsed = time.time() - start
                            print(f"[EmailBot] Sent {count} messages in {elapsed:.2f}s ({count/elapsed:.2f} msgs/sec)")
                        except (ValueError, IndexError):
                            print("[EmailBot] Usage: flood <number>")
                    else:
                        self.send_message(user_input)

                except KeyboardInterrupt:
                    break
                except EOFError:
                    break

        except Exception as e:
            print(f"[EmailBot] Error: {e}")
            import traceback
            traceback.print_exc()
        finally:
            self.running = False
            if self.sock:
                try:
                    self.send_raw("QUIT :EmailBot shutting down")
                    self.sock.close()
                except:
                    pass
            print("\n[EmailBot] Disconnected")


if __name__ == "__main__":
    print("="*70)
    print("IRC EmailBot - Demonstrating Oper Flood Bypass")
    print("="*70)

    bot = EmailBot()
    bot.run()
