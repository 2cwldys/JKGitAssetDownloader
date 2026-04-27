// cl_assets.cpp , Server asset auto-download
// Server broadcasts g_assetDownloadURL (GitHub repo or direct zip) + g_assetVersion/g_assetHash
// via SERVERINFO.  Client calls "cl_downloadserverassets <url> [version_or_hash]", which:
//   1. If GitHub URL  → hit releases/latest API, find .zip asset, resolve version
//   2. Compare version/hash against local cache , skip download if already current
//   3. Download zip to temp file (progress reported via cl_gitProgress 0-100)
//   4. Extract to <fs_homepath>/<fs_mbiii>/, stripping first path component
//   5. Cache version/hash so subsequent joins skip re-download
//   6. When all queued jobs finish, notify "Assets ready" , no restart needed (loose files load on-demand)
//
// cl_gitAutoDownload: 0 = never, 1 = auto on join (default)
// cl_flushserverassets [url] : clear cached version(s) so next join re-downloads

// 2C_ASSET_DOWNLOAD
#include "client.h"
#include "cl_assets.h"

static cvar_t *cl_gitStatus;          // "idle" | "checking" | "downloading" | "extracting" | "ready" | "error: ..."
static cvar_t *cl_gitProgress;        // "0"-"100"
static cvar_t *cl_gitAutoDownload;    // 0=never, 1=auto on join (default)  // 2C_ASSET_DOWNLOAD
static cvar_t *cl_gitPendingConnect;  // server address to auto-connect after queue drains

#ifdef _WIN32

#include <windows.h>
#include <wininet.h>
#include <minizip/unzip.h>
#include <stdlib.h>
#include <stdio.h>

#pragma comment(lib, "wininet.lib")

static HANDLE          s_dlThread = NULL;
static volatile LONG   s_dlDone   = 0;

typedef struct {
	char url[1024];
	char version[64];
} assetDlArgs_t;

#define MAX_ASSET_QUEUE 4
static assetDlArgs_t s_dlQueue[MAX_ASSET_QUEUE];
static int           s_dlQueueHead = 0; // next slot to read
static int           s_dlQueueTail = 0; // next slot to write
static assetDlArgs_t s_dlArgs;          // active job passed to thread

// ─── HTTP ────────────────────────────────────────────────────────────────────

// HTTP GET → heap buffer (caller frees). Returns NULL on failure.
static char *HTTP_GetBuffer(const char *url, int *outLen) {
	HINTERNET hNet = InternetOpenA("MBIII-Assets/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
	if (!hNet) return NULL;

	DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE |
	              INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI;
	HINTERNET hUrl = InternetOpenUrlA(hNet, url,
		"Accept: application/vnd.github+json\r\n", -1, flags, 0);
	if (!hUrl) { InternetCloseHandle(hNet); return NULL; }

	DWORD status = 0, statusLen = sizeof(status);
	HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
	               &status, &statusLen, NULL);
	if (status != 200) { InternetCloseHandle(hUrl); InternetCloseHandle(hNet); return NULL; }

	char   chunk[65536];
	DWORD  bytesRead;
	char  *buf    = NULL;
	int    total  = 0;

	while (InternetReadFile(hUrl, chunk, sizeof(chunk), &bytesRead) && bytesRead > 0) {
		buf = (char *)realloc(buf, total + (int)bytesRead + 1);
		memcpy(buf + total, chunk, bytesRead);
		total += (int)bytesRead;
	}
	if (buf) buf[total] = '\0';
	if (outLen) *outLen = total;

	InternetCloseHandle(hUrl);
	InternetCloseHandle(hNet);
	return buf;
}

// HTTP GET → write to file. Returns bytes written, or -1 on failure.
static int HTTP_GetFile(const char *url, const char *destPath, int *outTotal) {
	HINTERNET hNet = InternetOpenA("MBIII-Assets/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
	if (!hNet) return -1;

	DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_SECURE |
	              INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI;
	HINTERNET hUrl = InternetOpenUrlA(hNet, url, NULL, 0, flags, 0);
	if (!hUrl) { InternetCloseHandle(hNet); return -1; }

	DWORD contentLen = 0, clBufLen = sizeof(contentLen);
	HttpQueryInfoA(hUrl, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER,
	               &contentLen, &clBufLen, NULL);

	FILE *f = fopen(destPath, "wb");
	if (!f) { InternetCloseHandle(hUrl); InternetCloseHandle(hNet); return -1; }

	BYTE  chunk[65536];
	DWORD bytesRead;
	int   total = 0;
	char  progStr[8];

	while (InternetReadFile(hUrl, chunk, sizeof(chunk), &bytesRead) && bytesRead > 0) {
		fwrite(chunk, 1, bytesRead, f);
		total += (int)bytesRead;
		if (contentLen > 0) {
			int pct = (int)((double)total / contentLen * 100.0);
			Com_sprintf(progStr, sizeof(progStr), "%d", pct);
			Cvar_Set("cl_gitProgress", progStr);
		}
	}
	fclose(f);
	InternetCloseHandle(hUrl);
	InternetCloseHandle(hNet);
	if (outTotal) *outTotal = total;
	return total;
}

// ─── JSON scanner ────────────────────────────────────────────────────────────

// Extract value of first "key":"VALUE" pair starting at json.
static qboolean JSON_String(const char *json, const char *key, char *out, int outSize) {
	char pattern[256];
	Com_sprintf(pattern, sizeof(pattern), "\"%s\"", key);
	const char *p = strstr(json, pattern);
	if (!p) return qfalse;
	p += strlen(pattern);
	while (*p == ' ' || *p == '\t' || *p == ':' || *p == ' ') p++;
	if (*p != '"') return qfalse;
	p++;
	int i = 0;
	while (*p && *p != '"' && i < outSize - 1)
		out[i++] = *p++;
	out[i] = '\0';
	return i > 0 ? qtrue : qfalse;
}

// In GitHub releases JSON, find the browser_download_url of the first .zip asset.
static qboolean FindZipURL(const char *json, char *out, int outSize) {
	const char *p = json;
	while ((p = strstr(p, "\"name\"")) != NULL) {
		char name[256] = {0};
		JSON_String(p, "name", name, sizeof(name));
		int nl = (int)strlen(name);
		if (nl >= 4 && !Q_stricmpn(name + nl - 4, ".zip", 4)) {
			const char *urlPos = strstr(p, "\"browser_download_url\"");
			if (urlPos)
				return JSON_String(urlPos, "browser_download_url", out, outSize);
		}
		p++;
	}
	return qfalse;
}

// ─── GitHub URL parser ───────────────────────────────────────────────────────

// "https://github.com/owner/repo[/...]" → owner, repo
static qboolean ParseGitHubURL(const char *url, char *owner, int owSize, char *repo, int repoSize) {
	const char *p = strstr(url, "github.com/");
	if (!p) return qfalse;
	p += strlen("github.com/");
	const char *slash = strchr(p, '/');
	if (!slash) return qfalse;
	int n = (int)(slash - p);
	if (n <= 0 || n >= owSize) return qfalse;
	Q_strncpyz(owner, p, n + 1);
	owner[n] = '\0';
	Q_strncpyz(repo, slash + 1, repoSize);
	// strip .git suffix or trailing slash
	int rn = (int)strlen(repo);
	if (rn > 4 && !Q_stricmpn(repo + rn - 4, ".git", 4)) repo[rn - 4] = '\0';
	if (rn > 0 && repo[rn - 1] == '/') repo[rn - 1] = '\0';
	return qtrue;
}

// ─── Version cache ───────────────────────────────────────────────────────────
// Stored at <fs_homepath>/<cl_gitAssetDir>/asset_versions.cfg, one "url version" per line.

// Resolve the extraction/cache subdirectory.
// Priority: cl_gitAssetDir > fs_game > "base"
static void GetAssetDir(char *out, int outSize) {
	Cvar_VariableStringBuffer("cl_gitAssetDir", out, outSize);
	if (!out[0]) Cvar_VariableStringBuffer("fs_game", out, outSize);
	if (!out[0]) Q_strncpyz(out, "base", outSize);
}

static void GetCachePath(char *out, int outSize) {
	char home[MAX_OSPATH], assetDir[64];
	Cvar_VariableStringBuffer("fs_homepath", home, sizeof(home));
	GetAssetDir(assetDir, sizeof(assetDir));
	Com_sprintf(out, outSize, "%s/%s/asset_versions.cfg", home, assetDir);
}

static void ReadCachedVersion(const char *url, char *out, int outSize) {
	char path[MAX_OSPATH];
	GetCachePath(path, sizeof(path));
	out[0] = '\0';
	FILE *f = fopen(path, "r");
	if (!f) return;
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		char *sp = strchr(line, ' ');
		if (!sp) continue;
		*sp = '\0';
		if (!Q_stricmp(line, url)) {
			Q_strncpyz(out, sp + 1, outSize);
			int n = (int)strlen(out);
			while (n > 0 && (out[n-1] == '\r' || out[n-1] == '\n')) out[--n] = '\0';
			break;
		}
	}
	fclose(f);
}

static void WriteCachedVersion(const char *url, const char *version) {
	char path[MAX_OSPATH];
	GetCachePath(path, sizeof(path));

	// Read all existing lines, replacing or appending
	char **lines = NULL;
	int lineCount = 0;
	qboolean found = qfalse;
	FILE *f = fopen(path, "r");
	if (f) {
		char line[1024];
		while (fgets(line, sizeof(line), f)) {
			char *sp = strchr(line, ' ');
			if (sp) {
				char savedUrl[1024]; int n = (int)(sp - line);
				if (n > 0 && n < (int)sizeof(savedUrl)) {
					Q_strncpyz(savedUrl, line, n + 1);
					if (!Q_stricmp(savedUrl, url)) {
						char newLine[1100];
						Com_sprintf(newLine, sizeof(newLine), "%s %s\n", url, version);
						lines = (char **)realloc(lines, (lineCount + 1) * sizeof(char *));
						lines[lineCount++] = strdup(newLine);
						found = qtrue;
						continue;
					}
				}
			}
			lines = (char **)realloc(lines, (lineCount + 1) * sizeof(char *));
			lines[lineCount++] = strdup(line);
		}
		fclose(f);
	}
	if (!found) {
		char newLine[1100];
		Com_sprintf(newLine, sizeof(newLine), "%s %s\n", url, version);
		lines = (char **)realloc(lines, (lineCount + 1) * sizeof(char *));
		lines[lineCount++] = strdup(newLine);
	}
	f = fopen(path, "w");
	if (f) {
		for (int i = 0; i < lineCount; i++) { fputs(lines[i], f); free(lines[i]); }
		fclose(f);
	} else {
		for (int i = 0; i < lineCount; i++) free(lines[i]);
	}
	free(lines);
}

// ─── ZIP extraction ──────────────────────────────────────────────────────────

// Create all intermediate directories for a file path.
static void MakeDirs(const char *filePath) {
	char tmp[MAX_OSPATH];
	Q_strncpyz(tmp, filePath, sizeof(tmp));
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/' || *p == '\\') {
			char c = *p; *p = '\0';
			CreateDirectoryA(tmp, NULL);
			*p = c;
		}
	}
}

// Strip the first path component (common root dir) from a zip member name.
// e.g., "repo-v1.0/maps/file.bsp" → "maps/file.bsp"
// Returns NULL for directory entries or names with no content after strip.
static const char *StripRootDir(const char *name) {
	const char *sl = strchr(name, '/');
	if (!sl) return name;         // no subdirectory , keep as-is
	if (*(sl + 1) == '\0') return NULL; // this IS the root directory entry
	return sl + 1;
}

// Extract zip at zipPath into destDir, stripping the first path component.
// Returns number of files extracted, or -1 on open failure.
static int ExtractZip(const char *zipPath, const char *destDir) {
	unzFile uz = unzOpen(zipPath);
	if (!uz) return -1;

	unz_global_info gi;
	if (unzGetGlobalInfo(uz, &gi) != UNZ_OK) { unzClose(uz); return -1; }

	char nameBuf[MAX_OSPATH];
	char outPath[MAX_OSPATH];
	char readBuf[65536];
	char progStr[8];
	int  done = 0;

	for (uLong i = 0; i < gi.number_entry; i++) {
		unz_file_info fi;
		if (unzGetCurrentFileInfo(uz, &fi, nameBuf, sizeof(nameBuf),
		                          NULL, 0, NULL, 0) != UNZ_OK)
			break;

		const char *rel = StripRootDir(nameBuf);
		int nameLen = (int)strlen(nameBuf);

		if (rel && rel[0] && nameBuf[nameLen - 1] != '/') {
			Com_sprintf(outPath, sizeof(outPath), "%s/%s", destDir, rel);
			// normalize to forward slashes
			for (char *p = outPath; *p; p++) if (*p == '\\') *p = '/';

			MakeDirs(outPath);

			if (unzOpenCurrentFile(uz) == UNZ_OK) {
				FILE *fout = fopen(outPath, "wb");
				if (fout) {
					int n;
					while ((n = unzReadCurrentFile(uz, readBuf, sizeof(readBuf))) > 0)
						fwrite(readBuf, 1, n, fout);
					fclose(fout);
				}
				unzCloseCurrentFile(uz);
			}
			done++;
			if (gi.number_entry > 0) {
				int pct = (int)((double)done / gi.number_entry * 100.0);
				Com_sprintf(progStr, sizeof(progStr), "%d", pct);
				Cvar_Set("cl_gitProgress", progStr);
			}
		}

		if (i + 1 < gi.number_entry && unzGoToNextFile(uz) != UNZ_OK)
			break;
	}

	unzClose(uz);
	return done;
}

// ─── Download thread ─────────────────────────────────────────────────────────

static DWORD WINAPI AssetDownload_Thread(LPVOID param) {
	assetDlArgs_t *args = (assetDlArgs_t *)param;

	char homePath[MAX_OSPATH];
	char assetDir[64];
	char destDir[MAX_OSPATH];
	char tempZip[MAX_OSPATH];

	Cvar_VariableStringBuffer("fs_homepath", homePath, sizeof(homePath));
	GetAssetDir(assetDir, sizeof(assetDir));

	Com_sprintf(destDir, sizeof(destDir), "%s/%s", homePath, assetDir);
	Com_sprintf(tempZip, sizeof(tempZip), "%s/%s/_asset_tmp.zip", homePath, assetDir);

	// ── Step 1: Resolve zip URL ───────────────────────────────────────────────
	Cvar_Set("cl_gitStatus", "checking");

	char zipUrl[1024]         = {0};
	char resolvedVersion[128] = {0};

	if (strstr(args->url, "github.com")) {
		char owner[128], repo[128];
		if (!ParseGitHubURL(args->url, owner, sizeof(owner), repo, sizeof(repo))) {
			Cvar_Set("cl_gitStatus", "error: invalid GitHub URL");
			return 1;
		}

		char apiUrl[512];
		Com_sprintf(apiUrl, sizeof(apiUrl),
		            "https://api.github.com/repos/%s/%s/releases/latest", owner, repo);

		int jsonLen = 0;
		char *json = HTTP_GetBuffer(apiUrl, &jsonLen);
		if (!json || jsonLen == 0) {
			free(json);
			Cvar_Set("cl_gitStatus", "error: GitHub API unreachable");
			return 1;
		}

		JSON_String(json, "tag_name", resolvedVersion, sizeof(resolvedVersion));

		// Version check: if server version matches cached, skip
		if (resolvedVersion[0]) {
			char cached[128];
			ReadCachedVersion(args->url, cached, sizeof(cached));
			if (!Q_stricmp(cached, resolvedVersion)) {
				free(json);
				Cvar_Set("cl_gitStatus", "up-to-date");
				Com_Printf("^9[Assets] ^7Already on %s, skipping download.\n", resolvedVersion);
				return 0;
			}
		}

		qboolean found = FindZipURL(json, zipUrl, sizeof(zipUrl));
		free(json);

		if (!found || !zipUrl[0]) {
			Cvar_Set("cl_gitStatus", "error: no .zip in latest release");
			return 1;
		}

		Com_Printf("^9[Assets] ^7Resolved %s → %s (%s)\n", args->url, zipUrl, resolvedVersion);
	} else {
		// Direct zip URL
		Q_strncpyz(zipUrl, args->url, sizeof(zipUrl));
		Q_strncpyz(resolvedVersion, args->version[0] ? args->version : "unknown",
		           sizeof(resolvedVersion));

		// Version check for direct URLs
		if (resolvedVersion[0] && Q_stricmp(resolvedVersion, "unknown")) {
			char cached[128];
			ReadCachedVersion(args->url, cached, sizeof(cached));
			if (!Q_stricmp(cached, resolvedVersion)) {
				Cvar_Set("cl_gitStatus", "up-to-date");
				Com_Printf("^9[Assets] ^7Already on %s , skipping download.\n", resolvedVersion);
				return 0;
			}
		}
	}

	// ── Step 2: Download zip ─────────────────────────────────────────────────
	Cvar_Set("cl_gitStatus",   "downloading");
	Cvar_Set("cl_gitProgress", "0");
	Com_Printf("^9[Assets] ^7Downloading %s...\n", zipUrl);

	// Ensure the MBIII staging directory exists before writing the temp zip
	MakeDirs(tempZip);

	int totalBytes = 0;
	if (HTTP_GetFile(zipUrl, tempZip, &totalBytes) < 0 || totalBytes == 0) {
		DeleteFileA(tempZip);
		Cvar_Set("cl_gitStatus", "error: download failed");
		return 1;
	}
	Com_Printf("^9[Assets] ^7Downloaded %d bytes.\n", totalBytes);

	// ── Step 3: Extract ──────────────────────────────────────────────────────
	Cvar_Set("cl_gitStatus",   "extracting");
	Cvar_Set("cl_gitProgress", "0");
	Com_Printf("^9[Assets] ^7Extracting to %s...\n", destDir);

	int extracted = ExtractZip(tempZip, destDir);
	DeleteFileA(tempZip);

	if (extracted < 0) {
		Cvar_Set("cl_gitStatus", "error: extraction failed");
		return 1;
	}
	Com_Printf("^9[Assets] ^7Extracted %d files.\n", extracted);

	// ── Step 4: Cache version ────────────────────────────────────────────────
	WriteCachedVersion(args->url, resolvedVersion);

	// ── Step 5: Signal vid_restart via main thread ───────────────────────────
	Cvar_Set("cl_gitStatus",   "done");
	Cvar_Set("cl_gitProgress", "100");
	InterlockedExchange(&s_dlDone, 1);

	return 0;
}

// ─── Public API ──────────────────────────────────────────────────────────────

// Returns true while a download thread is running or jobs remain in the queue.
qboolean CL_AssetDownload_IsBusy(void) {
	if (s_dlQueueHead != s_dlQueueTail) return qtrue;
	if (s_dlThread && WaitForSingleObject(s_dlThread, 0) == WAIT_TIMEOUT) return qtrue;
	return qfalse;
}

static void AssetDownload_StartNext(void) {
	if (s_dlQueueHead == s_dlQueueTail) return; // queue empty
	if (s_dlThread && WaitForSingleObject(s_dlThread, 0) == WAIT_TIMEOUT) return; // still running
	if (s_dlThread) { CloseHandle(s_dlThread); s_dlThread = NULL; }

	s_dlArgs = s_dlQueue[s_dlQueueHead];
	s_dlQueueHead = (s_dlQueueHead + 1) % MAX_ASSET_QUEUE;

	Com_Printf("^9[Assets] ^7Starting download: %s (token: %s)\n",
	           s_dlArgs.url, s_dlArgs.version[0] ? s_dlArgs.version : "auto");

	InterlockedExchange(&s_dlDone, 0);
	s_dlThread = CreateThread(NULL, 0, AssetDownload_Thread, &s_dlArgs, 0, NULL);
	if (!s_dlThread)
		Cvar_Set("cl_gitStatus", "error: thread creation failed");
}

void CL_AssetDownload_Init(void) {
	cl_gitStatus          = Cvar_Get("cl_gitStatus",         "idle", CVAR_ROM,
	                                   "Server asset download status");
	cl_gitProgress        = Cvar_Get("cl_gitProgress",       "0",    CVAR_ROM,
	                                   "Server asset download progress 0-100");
	cl_gitAutoDownload    = Cvar_Get("cl_gitAutoDownload",   "0",    CVAR_ARCHIVE,
	                                   "Auto-download server assets on join: 0=never (use server browser button), 1=auto");
	cl_gitPendingConnect  = Cvar_Get("cl_gitPendingConnect", "",     CVAR_ROM,
	                                   "Server address to connect to once asset queue is fully drained");
	Cvar_Get("cl_gitAssetDir", "", CVAR_ARCHIVE,
	         "Subdirectory under fs_homepath where assets are extracted (default: fs_game, then 'base')");
}

void CL_AssetDownload_Frame(void) {
	if (InterlockedCompareExchange(&s_dlDone, 0, 1) == 1) {
		AssetDownload_StartNext();
		if (s_dlQueueHead == s_dlQueueTail) {
			// Entire queue drained
			Cvar_Set("cl_gitStatus", "ready");
			if (cl_gitPendingConnect && cl_gitPendingConnect->string[0]) {
				Com_Printf("^9[Assets] ^7Assets ready , connecting to %s.\n", cl_gitPendingConnect->string);
				Cbuf_ExecuteText(EXEC_APPEND, va("connect %s\n", cl_gitPendingConnect->string));
				Cvar_Set("cl_gitPendingConnect", "");
			} else {
				Com_Printf("^9[Assets] ^7Assets ready. You can now join the server.\n");
			}
		}
	}
}

void CL_AssetDownload_f(void) {
	if (Cmd_Argc() < 2) {
		Com_Printf("Usage: cl_downloadserverassets <url> [version]\n"
		           "  url     : GitHub repo (https://github.com/owner/repo) or direct .zip URL\n"
		           "  version : optional tag to skip re-download if already cached\n");
		return;
	}

	// Enqueue the job
	int nextTail = (s_dlQueueTail + 1) % MAX_ASSET_QUEUE;
	if (nextTail == s_dlQueueHead) {
		Com_Printf("^9[Assets] ^7Download queue full , skipping %s.\n", Cmd_Argv(1));
		return;
	}
	Q_strncpyz(s_dlQueue[s_dlQueueTail].url,     Cmd_Argv(1),                         sizeof(s_dlQueue[0].url));
	Q_strncpyz(s_dlQueue[s_dlQueueTail].version, Cmd_Argc() >= 3 ? Cmd_Argv(2) : "", sizeof(s_dlQueue[0].version));
	s_dlQueueTail = nextTail;

	// Start immediately if idle
	AssetDownload_StartNext();
}

// Clear cached version(s) so the next join triggers a fresh download.
// Usage: cl_flushserverassets [url]
//   No url → clear all cached entries (delete asset_versions.cfg)
//   With url → remove only that URL's entry
void CL_AssetFlush_f(void) {
	char path[MAX_OSPATH];
	GetCachePath(path, sizeof(path));

	if (Cmd_Argc() < 2) {
		// Flush everything
		if (DeleteFileA(path))
			Com_Printf("^9[Assets] ^7Cache flushed , all entries removed.\n");
		else
			Com_Printf("^9[Assets] ^7Nothing to flush (cache not found).\n");
		return;
	}

	const char *targetUrl = Cmd_Argv(1);

	// Read all existing lines, drop the one that matches
	FILE *f = fopen(path, "r");
	if (!f) {
		Com_Printf("^9[Assets] ^7Nothing to flush (cache not found).\n");
		return;
	}

	char  **lines     = NULL;
	int     lineCount = 0;
	qboolean removed  = qfalse;
	char    line[1024];

	while (fgets(line, sizeof(line), f)) {
		char *sp = strchr(line, ' ');
		if (sp) {
			char savedUrl[1024]; int n = (int)(sp - line);
			if (n > 0 && n < (int)sizeof(savedUrl)) {
				Q_strncpyz(savedUrl, line, n + 1);
				if (!Q_stricmp(savedUrl, targetUrl)) { removed = qtrue; continue; }
			}
		}
		lines = (char **)realloc(lines, (lineCount + 1) * sizeof(char *));
		lines[lineCount++] = strdup(line);
	}
	fclose(f);

	FILE *fw = fopen(path, "w");
	if (fw) {
		for (int i = 0; i < lineCount; i++) { fputs(lines[i], fw); free(lines[i]); }
		fclose(fw);
	} else {
		for (int i = 0; i < lineCount; i++) free(lines[i]);
	}
	free(lines);

	if (removed)
		Com_Printf("^9[Assets] ^7Flushed cache entry for: %s\n", targetUrl);
	else
		Com_Printf("^9[Assets] ^7No cache entry found for: %s\n", targetUrl);
}

#else // Non-Windows stub

void CL_AssetDownload_Init(void) {
	Cvar_Get("cl_gitStatus",         "idle", CVAR_ROM,     "Server asset download status");
	Cvar_Get("cl_gitProgress",       "0",    CVAR_ROM,     "Server asset download progress 0-100");
	Cvar_Get("cl_gitAutoDownload",   "0",    CVAR_ARCHIVE, "Auto-download server assets on join: 0=never, 1=auto");
	Cvar_Get("cl_gitPendingConnect", "",     CVAR_ROM,     "Server address to connect to once asset queue drains");
	Cvar_Get("cl_gitAssetDir",       "",     CVAR_ARCHIVE, "Subdirectory under fs_homepath where assets are extracted (default: fs_game, then 'base')");
}
void CL_AssetDownload_Frame(void) {}
qboolean CL_AssetDownload_IsBusy(void) { return qfalse; }
void CL_AssetDownload_f(void) {
	Com_Printf("^9[Assets] ^7Asset download not yet supported on this platform.\n");
}
void CL_AssetFlush_f(void) {
	Com_Printf("^9[Assets] ^7Asset flush not yet supported on this platform.\n");
}

#endif
