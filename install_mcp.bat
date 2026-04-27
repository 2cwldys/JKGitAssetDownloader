@echo off
pip install -r "%~dp0requirements.txt"
if errorlevel 1 (
    echo ERROR: pip install failed.
    pause
    exit /b 1
)
claude mcp add git-asset-downloader -- python "%~dp0git_assistant.py"
if errorlevel 1 (
    echo ERROR: MCP registration failed.
    pause
    exit /b 1
)
echo.
echo git-asset-downloader MCP server registered. Restart Claude Code to activate.
pause
