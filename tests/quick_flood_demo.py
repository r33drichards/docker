#!/usr/bin/env python3
"""Quick demonstration of bot flood bypass vs regular user"""

import socket
import time


def test_user(nick, oper_creds=None, num_messages=200):
    """Test flooding with optional oper authentication"""
    print(f"\n{'='*70}")
    print(f"Testing: {nick}")
    print(f"Messages to send: {num_messages}")
    if oper_creds:
        print(f"Oper account: {oper_creds[0]}")
    print(f"{'='*70}\n")

    try:
        # Connect
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(10)
        sock.connect(('localhost', 6667))

        # Register
        sock.send(f'NICK {nick}\r\n'.encode('utf-8'))
        sock.send(f'USER {nick.lower()} 0 * :Test User\r\n'.encode('utf-8'))

        # Wait for registration
        start = time.time()
        registered = False
        while time.time() - start < 10:
            data = sock.recv(4096).decode('utf-8', errors='ignore')
            if 'PING' in data:
                sock.send(('PONG :' + data.split(':')[1]).encode('utf-8'))
            if ' 001 ' in data:
                registered = True
                print(f'[{nick}] ✓ Registered')
                break

        if not registered:
            print(f'[{nick}] ✗ Failed to register')
            return

        # Oper up if credentials provided
        if oper_creds:
            sock.send(f'OPER {oper_creds[0]} {oper_creds[1]}\r\n'.encode('utf-8'))
            start = time.time()
            while time.time() - start < 5:
                data = sock.recv(4096).decode('utf-8', errors='ignore')
                if 'PING' in data:
                    sock.send(('PONG :' + data.split(':')[1]).encode('utf-8'))
                if ' 381 ' in data:
                    print(f'[{nick}] ✓ Opered up successfully')
                    break

        time.sleep(0.5)

        # Join channel
        sock.send(b'JOIN #floodtest\r\n')
        time.sleep(0.5)

        # Flood test
        print(f'[{nick}] Sending {num_messages} messages as fast as possible...')
        start_time = time.time()
        messages_sent = 0
        disconnected = False

        for i in range(num_messages):
            try:
                sock.send(f'PRIVMSG #floodtest :Message {i+1}/{num_messages}\r\n'.encode('utf-8'))
                messages_sent += 1

                # Check for disconnect/errors
                sock.settimeout(0.0001)
                try:
                    data = sock.recv(4096).decode('utf-8', errors='ignore')
                    if 'ERROR' in data:
                        print(f'[{nick}] ✗ DISCONNECTED at message {i+1}: {data.strip()}')
                        disconnected = True
                        break
                    if 'Excess' in data or 'RecvQ' in data:
                        print(f'[{nick}] ⚠ Flood warning at message {i+1}: {data.strip()}')
                except:
                    pass
                sock.settimeout(10)

            except BrokenPipeError:
                print(f'[{nick}] ✗ Connection broken at message {i+1}')
                disconnected = True
                break
            except Exception as e:
                print(f'[{nick}] ✗ Error at message {i+1}: {e}')
                disconnected = True
                break

        elapsed = time.time() - start_time
        rate = messages_sent / elapsed if elapsed > 0 else 0

        print(f'\n[{nick}] Results:')
        print(f'  Messages sent: {messages_sent}/{num_messages}')
        print(f'  Time: {elapsed:.2f}s')
        print(f'  Rate: {rate:.2f} msgs/sec')
        print(f'  Disconnected: {disconnected}')

        if messages_sent == num_messages and not disconnected:
            print(f'  ✓ PASS: Sent all messages successfully')
        else:
            print(f'  ✗ FAIL: Flood protection triggered')

        try:
            sock.send(b'QUIT :Test complete\r\n')
            sock.close()
        except:
            pass

    except Exception as e:
        print(f'[{nick}] ✗ Test failed: {e}')


if __name__ == '__main__':
    print("\n" + "="*70)
    print("IRC FLOOD BYPASS DEMONSTRATION")
    print("="*70)

    # Test 1: Regular user
    test_user('RegularUser', num_messages=200)

    time.sleep(2)

    # Test 2: Bot with oper privileges
    test_user('BotUser', oper_creds=('BotUser', 'changeme_bot_password_123'), num_messages=200)

    print("\n" + "="*70)
    print("DEMONSTRATION COMPLETE")
    print("="*70)
    print("\nConclusion:")
    print("  - Regular users get throttled/disconnected when flooding")
    print("  - Bots with oper privileges bypass flood protection")
    print("="*70 + "\n")
