JKGitAssetDownloader
====================
Server-side GitHub/zip asset auto-download for Jedi Academy mods.
Servers advertise up to 4 download URLs via SERVERINFO CVars; clients
download, extract, and version-cache each asset pack automatically.

Supports GitHub repo URLs (fetches latest release .zip via API) and
direct .zip URLs. Downloads run on a background thread with no stutter.
Up to 4 slots queue sequentially. Version caching skips re-downloads.


FILE INVENTORY
--------------
cl_assets.cpp       Full implementation  — copy to codemp/client/
cl_assets.h         Header              — copy to codemp/client/
cl_main.cpp         Code snippets       — merge into codemp/client/cl_main.cpp
g_xcvar.h           CVar definitions    — merge into source/game/g_xcvar.h
ui_main.c           UI code hunks       — merge into source/ui/ui_main.c
mb_serverinfo.menu  Menu override       — copy to ui/mb/alpha/share/ in your PK3


INTEGRATION STEPS
-----------------

1. COPY NEW FILES
   Copy cl_assets.cpp and cl_assets.h into codemp/client/.

2. ENGINE CMAKELISTS (codemp/CMakeLists.txt)
   In the MPEngineClientFiles list, add:
       "${MPDir}/client/cl_assets.cpp"
       "${MPDir}/client/cl_assets.h"

3. ENGINE INIT / FRAME (codemp/client/cl_main.cpp)
   See cl_main.cpp in this folder.  Three call sites:
     a) #include "cl_assets.h" at the top
     b) CL_AssetDownload_Init() in CL_Init()
     c) CL_AssetDownload_Frame() in CL_Frame()
   And two command registrations in CL_Init():
     cl_downloadserverassets / cl_flushserverassets

4. SERVER CVARS (source/game/g_xcvar.h)
   Append the 8 XCVAR_DEF lines from g_xcvar.h in this folder to your
   g_xcvar.h CVar block. These are the g_gitDownloadURL/Version slots 1-4.

5. UI INTEGRATION (source/ui/ui_main.c)
   See ui_main.c in this folder.  Four hunks:
     HUNK 1 — DownloadServerAssets + CheckServerAssets uiScript handlers
     HUNK 2 — reset ui_serverHasAssets in "ServerStatus" handler
     HUNK 3 — async scan in UI_BuildServerStatus() when getstatus data arrives
     HUNK 4 — block/defer connect in UI_JoinServer() while download runs

6. SERVER BROWSER MENU (OPTIONAL - For MB builds)
   Copy mb_serverinfo.menu into your mod's ui/mb/alpha/share/ directory
   (inside a PK3).  It adds a "DOWNLOAD ASSETS" button that appears only
   when ui_serverHasAssets == "1".
   Also add to your menu script (.txt):
       loadMenu { "ui/mb/alpha/share/mb_serverinfo.menu" }

7. SERVER CONFIGURATION
   On the server, set the CVars you want clients to download:
       set g_gitDownloadURL  "https://github.com/owner/repo"
       set g_gitVersion      "v1.0.0"          // optional; skips re-download
       // slots 2-4 are optional:
       set g_gitDownloadURL2 "https://example.com/extra.zip"
       set g_gitVersion2     "2025-04-01"


CVars (client-side)
-------------------
cl_gitAutoDownload  0    Auto-download on join (0=off, 1=on). Default off.
cl_gitStatus        ROM  Download status: idle/checking/downloading/extracting/ready/error
cl_gitProgress      ROM  Download progress 0-100
cl_gitAssetDir      ""   Subdirectory under fs_homepath where assets are extracted.
                         Resolution order:
                           1. cl_gitAssetDir (if set by the user/config)
                           2. fs_game        (the active mod folder, e.g. "MBII")
                           3. "base"         (final fallback)
                         Set this explicitly on the client if you want a fixed target:
                           cl_gitAssetDir "mymods"
                         Version cache is written to the same subdirectory:
                           <fs_homepath>/<cl_gitAssetDir>/asset_versions.cfg

Commands (client-side)
-----------------------
cl_downloadserverassets <url> [version]   Manually enqueue one download
cl_flushserverassets [url]                Clear cached version(s)


IMPORTANT NOTES
---------------
- URLs are read from the async getstatus response, NOT the browser cache.
  The browser info string is truncated at ~1024 bytes and will not contain
  long g_gitDownloadURL values.  The fix is already in the code — do not
  change it to use trap_LAN_GetServerInfo.
- JKA treats // as a comment in console commands. URLs are always passed
  double-quoted ("url") in trap_Cmd_ExecuteText calls. Do not remove quotes.
- No vid_restart needed. JKA picks up loose files on-demand.
