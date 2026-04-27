/*
 * JKGitAssetDownloader — g_xcvar.h additions
 * Add these lines to your mod's g_xcvar.h XCVAR_DEF block.
 * These CVars are broadcast via SERVERINFO so clients can see them in the
 * server browser getstatus response. The UI reads them to enqueue downloads.
 */

// 2C_ASSET_DOWNLOAD — server asset auto-download CVars (slots 1-4, run in sequence on the client)
// g_gitDownloadURL/2/3/4: GitHub repo URL or direct .zip URL.
// g_gitVersion/2/3/4:     release version tag — clients skip re-download when already current.
XCVAR_DEF(g_gitDownloadURL,  "g_gitDownloadURL",  "", CVAR_SERVERINFO | CVAR_ARCHIVE, 0, qfalse)
XCVAR_DEF(g_gitVersion,      "g_gitVersion",      "", CVAR_SERVERINFO | CVAR_ARCHIVE, 0, qfalse)
XCVAR_DEF(g_gitDownloadURL2, "g_gitDownloadURL2", "", CVAR_SERVERINFO | CVAR_ARCHIVE, 0, qfalse)
XCVAR_DEF(g_gitVersion2,     "g_gitVersion2",     "", CVAR_SERVERINFO | CVAR_ARCHIVE, 0, qfalse)
XCVAR_DEF(g_gitDownloadURL3, "g_gitDownloadURL3", "", CVAR_SERVERINFO | CVAR_ARCHIVE, 0, qfalse)
XCVAR_DEF(g_gitVersion3,     "g_gitVersion3",     "", CVAR_SERVERINFO | CVAR_ARCHIVE, 0, qfalse)
XCVAR_DEF(g_gitDownloadURL4, "g_gitDownloadURL4", "", CVAR_SERVERINFO | CVAR_ARCHIVE, 0, qfalse)
XCVAR_DEF(g_gitVersion4,     "g_gitVersion4",     "", CVAR_SERVERINFO | CVAR_ARCHIVE, 0, qfalse)
