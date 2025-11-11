#!/bin/bash
#
# Quick test script for bot flood configuration
#
# This script helps you quickly test the bot oper flood bypass
# against a running InspIRCd instance (local or remote).
#

set -e

# Default values
HOST="${IRC_HOST:-localhost}"
PORT="${IRC_PORT:-6667}"
BOT_PASSWORD="${BOT_PASSWORD:-changeme_bot_password_123}"
MESSAGES="${MESSAGES:-100}"
DELAY="${DELAY:-0.01}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "======================================================================"
echo "InspIRCd Bot Flood Test"
echo "======================================================================"
echo ""

# Check if Python 3 is available
if ! command -v python3 &> /dev/null; then
    echo -e "${RED}ERROR: python3 is not installed${NC}"
    echo "Please install Python 3 to run this test"
    exit 1
fi

# Check if server is reachable
echo -e "${YELLOW}Checking if IRC server is reachable...${NC}"
# Use nc (netcat) which is available on both Linux and macOS
if command -v nc &> /dev/null; then
    if nc -z -w 2 "$HOST" "$PORT" 2>/dev/null; then
        echo -e "${GREEN}✓ Server is reachable at $HOST:$PORT${NC}"
    else
        echo -e "${RED}✗ Cannot connect to $HOST:$PORT${NC}"
        echo ""
        echo "Make sure your InspIRCd server is running."
        echo ""
        echo "If using Docker, you can start it with:"
        echo "  docker run -d -p 6667:6667 -p 6697:6697 \\"
        echo "    -v \$(pwd)/conf:/inspircd/conf \\"
        echo "    inspircd/inspircd-docker"
        echo ""
        exit 1
    fi
else
    # Fallback to /dev/tcp if nc is not available
    if (echo > /dev/tcp/$HOST/$PORT) 2>/dev/null; then
        echo -e "${GREEN}✓ Server is reachable at $HOST:$PORT${NC}"
    else
        echo -e "${RED}✗ Cannot connect to $HOST:$PORT${NC}"
        echo ""
        echo "Make sure your InspIRCd server is running."
        echo ""
        echo "If using Docker, you can start it with:"
        echo "  docker run -d -p 6667:6667 -p 6697:6697 \\"
        echo "    -v \$(pwd)/conf:/inspircd/conf \\"
        echo "    inspircd/inspircd-docker"
        echo ""
        exit 1
    fi
fi

echo ""
echo "Test configuration:"
echo "  Server: $HOST:$PORT"
echo "  Bot password: $BOT_PASSWORD"
echo "  Messages to send: $MESSAGES"
echo "  Delay between messages: ${DELAY}s"
echo ""
echo -e "${YELLOW}NOTE: Make sure bot-oper.conf is included in your InspIRCd config!${NC}"
echo ""

# Ask for confirmation
read -p "Continue with test? [y/N] " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Test cancelled"
    exit 0
fi

echo ""
echo "======================================================================"
echo "Starting test..."
echo "======================================================================"
echo ""

# Run the Python test script
cd "$(dirname "$0")"
python3 test_bot_flood.py \
    --host "$HOST" \
    --port "$PORT" \
    --bot-password "$BOT_PASSWORD" \
    --messages "$MESSAGES" \
    --delay "$DELAY"

exit_code=$?

echo ""
echo "======================================================================"
if [ $exit_code -eq 0 ]; then
    echo -e "${GREEN}Test completed successfully!${NC}"
else
    echo -e "${RED}Test failed with exit code $exit_code${NC}"
fi
echo "======================================================================"

exit $exit_code
