#!/usr/bin/env bash
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
pip install -r "$SCRIPT_DIR/requirements.txt"
claude mcp add git-asset-downloader -- python "$SCRIPT_DIR/git_assistant.py"
echo "git-asset-downloader MCP server registered. Restart Claude Code to activate."
