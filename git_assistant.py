"""
JKGitAssetDownloader MCP Server for Jedi Academy Modding
Copyright (C) 2025 2cwldys — GNU GPL v2 or later

Exposes the JKGitAssetDownloader source files as resources and provides
analysis tools that identify exactly where to add asset-download hooks in
an existing JKA mod codebase.  Runs fully offline over stdio.

Add to Claude Code:
  claude mcp add git-asset-downloader -- python /path/to/git_assistant.py
"""

import os
import re
from mcp.server.fastmcp import FastMCP

mcp = FastMCP("JKA Git Asset Downloader Assistant")

GIT_DIR = os.path.dirname(os.path.abspath(__file__))

SOURCE_FILES = {
    "cl_assets.cpp":      "Full engine implementation — HTTP download thread, GitHub API resolver, zip extractor, version cache, queue",
    "cl_assets.h":        "Public header — CL_AssetDownload_Init/Frame/f, CL_AssetFlush_f, CL_AssetDownload_IsBusy",
    "cl_main.cpp":        "Engine hook points — include, init, frame, and command registration snippets",
    "g_xcvar.h":          "Server CVar definitions — g_gitDownloadURL/Version slots 1-4 (XCVAR_DEF entries)",
    "ui_main.c":          "UI code hunks — DownloadServerAssets, CheckServerAssets, UI_BuildServerStatus hook, UI_JoinServer gate",
    "mb_serverinfo.menu": "Server browser popup menu override with conditional DOWNLOAD ASSETS button",
    "readme.txt":         "Integration overview, ordered step list, CVar/command reference",
}

# Maps file basename patterns to their hook analysis rules.
# Each rule: (search_pattern, found_message, not_found_message, code_to_add)
HOOK_RULES = {
    "cl_main.cpp": [
        (
            r'#include\s+"cl_uiapi\.h"|#include\s+"cl_lan\.h"',
            "Found client headers — add #include \"cl_assets.h\" alongside them.",
            "No adjacent client header found — add #include \"cl_assets.h\" near the top of cl_main.cpp.",
            '#include "cl_assets.h"  // 2C_ASSET_DOWNLOAD'
        ),
        (
            r"CL_Init\s*\(",
            "Found CL_Init — add CL_AssetDownload_Init() inside it alongside other subsystem inits.",
            "CL_Init not found — locate your client initialisation function.",
            "CL_AssetDownload_Init();  // 2C_ASSET_DOWNLOAD"
        ),
        (
            r"Cmd_AddCommand\s*\(",
            "Found Cmd_AddCommand calls — add the two asset downloader commands alongside them.",
            "No Cmd_AddCommand found — locate where client commands are registered.",
            'Cmd_AddCommand("cl_downloadserverassets", CL_AssetDownload_f,  "Download and install server asset pack from URL");  // 2C_ASSET_DOWNLOAD\n'
            'Cmd_AddCommand("cl_flushserverassets",    CL_AssetFlush_f,     "Clear cached asset version(s); next join will re-download");  // 2C_ASSET_DOWNLOAD'
        ),
        (
            r"CL_Frame\s*\(",
            "Found CL_Frame — add CL_AssetDownload_Frame() inside it alongside other per-frame calls.",
            "CL_Frame not found — locate your client per-frame function.",
            "CL_AssetDownload_Frame();  // 2C_ASSET_DOWNLOAD"
        ),
    ],
    "g_xcvar.h": [
        (
            r"XCVAR_DEF",
            "Found XCVAR_DEF macro usage — append the 8 g_git* entries from JKGitAssetDownloader/g_xcvar.h to this block.",
            "No XCVAR_DEF found — register the 8 CVars manually with trap_Cvar_Register in G_InitGame.",
            None
        ),
        (
            r"CVAR_SERVERINFO",
            "Found CVAR_SERVERINFO usage — the g_git* CVars must also use CVAR_SERVERINFO so clients see them in getstatus.",
            None,
            None
        ),
    ],
    "ui_main.c": [
        (
            r'Q_stricmp\s*\(\s*name\s*,\s*"ServerStatus"\s*\)',
            'Found "ServerStatus" uiScript handler — add the ui_serverHasAssets reset line (HUNK 2) inside it after UI_BuildServerStatus(qtrue).',
            'No "ServerStatus" uiScript found — locate where server status is requested and add the reset.',
            'trap_Cvar_Set("ui_serverHasAssets", "0");  // 2C_ASSET_DOWNLOAD: reset; set async in UI_BuildServerStatus'
        ),
        (
            r"UI_GetServerStatusInfo\s*\(",
            "Found UI_GetServerStatusInfo — add the async asset URL scan (HUNK 3) immediately after the block where it returns true.",
            "UI_GetServerStatusInfo not found — locate your server status polling function.",
            None
        ),
        (
            r"uiInfo\.serverStatusInfo\.numLines|serverStatusInfo\.lines",
            "Found serverStatusInfo.lines access — the async scan in HUNK 3 reads from here. Verify it is inside the UI_GetServerStatusInfo-true branch.",
            "No serverStatusInfo.lines access found — the async URL scan must be placed where getstatus data has arrived.",
            None
        ),
        (
            r'Q_stricmp\s*\(\s*name\s*,\s*"JoinServer"\s*\)|static\s+void\s+UI_JoinServer',
            "Found UI_JoinServer — add the download-in-progress gate and auto-download block (HUNK 4) before the final connect call.",
            "UI_JoinServer not found — locate where the JOIN button fires the connect command.",
            None
        ),
        (
            r"trap_LAN_GetServerInfo\s*\(",
            "Found trap_LAN_GetServerInfo — WARNING: do NOT use this to read g_gitDownloadURL. "
            "The browser info string is truncated at ~1024 bytes. Always read from uiInfo.serverStatusInfo.lines instead.",
            None,
            None
        ),
    ],
    "cmakelists.txt": [
        (
            r"cl_main\.cpp|cl_avi\.cpp",
            "Found engine client file list — add cl_assets.cpp and cl_assets.h to this same list.",
            "No engine client files found in this CMakeLists — check you are in codemp/CMakeLists.txt.",
            '"${MPDir}/client/cl_assets.cpp"  # 2C_ASSET_DOWNLOAD\n"${MPDir}/client/cl_assets.h"   # 2C_ASSET_DOWNLOAD'
        ),
        (
            r"wininet|WININET",
            "Found wininet — cl_assets.cpp links wininet via #pragma comment(lib) on MSVC. No extra CMake change needed.",
            "No wininet reference found — on non-MSVC builds you may need to add wininet to target_link_libraries.",
            None
        ),
        (
            r"minizip|MINIZIP",
            "Found minizip — cl_assets.cpp includes <minizip/unzip.h>. Ensure MINIZIP_INCLUDE_DIRS covers this path.",
            "No minizip reference found — add the minizip include and library to the engine target (cl_assets.cpp requires it).",
            None
        ),
    ],
}


def _read_file(filename: str) -> str:
    path = os.path.join(GIT_DIR, filename)
    if not os.path.isfile(path):
        return f"File not found: {filename}"
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


@mcp.resource("git-assets://files/{filename}")
def git_asset_file(filename: str) -> str:
    """Return the content of a JKGitAssetDownloader source file."""
    if filename not in SOURCE_FILES:
        return f"Unknown file '{filename}'. Call list_files() to see available files."
    return _read_file(filename)


@mcp.tool()
def list_files() -> str:
    """List all JKGitAssetDownloader source files with a description of each."""
    lines = ["Available JKGitAssetDownloader files:\n"]
    for name, desc in SOURCE_FILES.items():
        lines.append(f"  {name}\n    {desc}\n")
    return "\n".join(lines)


@mcp.tool()
def read_git_file(filename: str) -> str:
    """
    Return the full content of a JKGitAssetDownloader source file.

    Args:
        filename: One of the files listed by list_files().
    """
    if filename not in SOURCE_FILES:
        return f"Unknown file '{filename}'. Call list_files() to see available files."
    return _read_file(filename)


@mcp.tool()
def analyze_mod_file(filename: str, content: str) -> str:
    """
    Analyze one of your existing mod source files and report exactly where
    to add JKGitAssetDownloader hooks, with the code to insert at each point.

    Args:
        filename: The basename of your file (e.g. "cl_main.cpp", "ui_main.c").
        content:  The full text of that file.
    """
    basename = os.path.basename(filename).lower()

    rules = None
    for key in HOOK_RULES:
        if basename == key:
            rules = HOOK_RULES[key]
            break

    if rules is None:
        supported = ", ".join(HOOK_RULES.keys())
        return (
            f"No analysis rules for '{basename}'.\n"
            f"Supported files: {supported}\n\n"
            "For other files, read the relevant JKGitAssetDownloader source file "
            "with read_git_file() and merge manually."
        )

    results = []
    for pattern, found_msg, not_found_msg, code in rules:
        match = re.search(pattern, content, re.IGNORECASE)
        if match:
            line_num = content[: match.start()].count("\n") + 1
            msg = f"[line ~{line_num}] {found_msg}"
            if code:
                msg += f"\n  Add:\n    {code.replace(chr(10), chr(10) + '    ')}"
        else:
            if not_found_msg:
                msg = f"[NOT FOUND] {not_found_msg}"
                if code:
                    msg += f"\n  Add:\n    {code.replace(chr(10), chr(10) + '    ')}"
            else:
                continue
        results.append(msg)

    if not results:
        return f"No integration points identified for '{basename}'."

    header = f"Analysis of {basename}:\n" + ("─" * 60) + "\n"
    return header + "\n\n".join(results)


@mcp.tool()
def get_integration_checklist() -> str:
    """Return the full ordered integration checklist for adding git asset downloading to a JKA mod."""
    return """
JKGitAssetDownloader — Integration Checklist
=============================================

ENGINE SIDE (codemp/)
---------------------
1. Copy files
   • cl_assets.cpp  →  codemp/client/cl_assets.cpp
   • cl_assets.h    →  codemp/client/cl_assets.h

2. codemp/CMakeLists.txt
   • In MPEngineClientFiles, add:
       "${MPDir}/client/cl_assets.cpp"
       "${MPDir}/client/cl_assets.h"
   • Verify minizip include dirs cover <minizip/unzip.h>.
   • On MSVC, wininet.lib is linked via #pragma — no CMake change needed.

3. codemp/client/cl_main.cpp
   • Add:  #include "cl_assets.h"
   • In CL_Init():  CL_AssetDownload_Init()
   • In CL_Init():  Cmd_AddCommand for cl_downloadserverassets and cl_flushserverassets
   • In CL_Frame(): CL_AssetDownload_Frame()
   Full snippets in JKGitAssetDownloader/cl_main.cpp.

SERVER SIDE (source/game/)
--------------------------
4. source/game/g_xcvar.h
   • Append the 8 XCVAR_DEF lines for g_gitDownloadURL/Version slots 1-4.
   • All 8 must be CVAR_SERVERINFO so getstatus broadcasts them to clients.
   Full entries in JKGitAssetDownloader/g_xcvar.h.

UI SIDE (source/ui/)
---------------------
5. source/ui/ui_main.c  — four hunks (search 2C_ASSET_DOWNLOAD):
   HUNK 1  Add DownloadServerAssets + CheckServerAssets uiScript handlers.
   HUNK 2  In "ServerStatus" handler: reset ui_serverHasAssets to "0".
   HUNK 3  In UI_BuildServerStatus(): async scan for g_gitDownloadURL* when
           UI_GetServerStatusInfo() returns true.
   HUNK 4  In UI_JoinServer(): block connect while downloading; auto-download
           when cl_gitAutoDownload is set.
   Full code in JKGitAssetDownloader/ui_main.c.

MENU / ASSETS
-------------
6. mb_serverinfo.menu
   • Copy JKGitAssetDownloader/mb_serverinfo.menu into your mod's
     ui/mb/alpha/share/ (inside a pk3).
   • Ensure your menu script (.txt) loads it:
       loadMenu { "ui/mb/alpha/share/mb_serverinfo.menu" }
   • The DOWNLOAD ASSETS button is gated on ui_serverHasAssets == "1"
     and fires uiScript DownloadServerAssets.

SERVER CONFIGURATION
--------------------
7. On your server, set the CVars you want clients to fetch:
       set g_gitDownloadURL  "https://github.com/owner/repo"
       set g_gitVersion      "v1.0.0"
   Slots 2-4 are optional:
       set g_gitDownloadURL2 "https://example.com/extra.zip"
       set g_gitVersion2     "2025-04-26"

CLIENT CVars
------------
  cl_gitAutoDownload   0    Auto-download on join (0=off, 1=on). Default off.
  cl_gitStatus         ROM  idle / checking / downloading / extracting / ready / error:...
  cl_gitProgress       ROM  0-100
  cl_gitAssetDir       ""   Extraction subdirectory under fs_homepath.
                            Falls back to fs_game then "base" if unset.

Client Commands
---------------
  cl_downloadserverassets <url> [version]   Manually enqueue one URL
  cl_flushserverassets [url]                Clear cached version(s)

IMPORTANT GOTCHAS
-----------------
• NEVER read g_gitDownloadURL from trap_LAN_GetServerInfo — the browser
  cache is truncated at ~1024 bytes. Always read from uiInfo.serverStatusInfo.lines
  (the async getstatus response). The provided code already does this correctly.
• JKA treats // as a comment in console commands. URLs must be double-quoted
  in all trap_Cmd_ExecuteText calls. The provided code already does this.
• No vid_restart needed after extraction. JKA picks up loose files on-demand.
• Extraction target: <fs_homepath>/<cl_gitAssetDir>/
  Resolution order: cl_gitAssetDir → fs_game → "base".
  Set cl_gitAssetDir on the client to pin a specific folder.

Use analyze_mod_file(filename, content) to get line-specific guidance for
each of your existing source files.
""".strip()
