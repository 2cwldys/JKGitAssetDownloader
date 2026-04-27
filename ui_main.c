/*
 * JKGitAssetDownloader — ui_main.c additions
 * Search "2C_ASSET_DOWNLOAD" in ui_main.c to find each integration point.
 *
 * HUNK 1 — add to UI_RunMenuScript() uiScript dispatch chain
 * HUNK 2 — add inside the existing "ServerStatus" uiScript handler
 * HUNK 3 — add inside UI_BuildServerStatus() after UI_GetServerStatusInfo() returns true
 * HUNK 4 — add inside UI_JoinServer() before the final connect call
 */

// ─── HUNK 1 — UI_RunMenuScript uiScript dispatch ─────────────────────────────

		} else if (Q_stricmp(name, "DownloadServerAssets") == 0) { // 2C_ASSET_DOWNLOAD
			// Reads URLs from the full async getstatus response (serverStatusInfo.lines),
			// NOT trap_LAN_GetServerInfo — browser cache is truncated and never has long CVars.
			{
				static const char *urlKeys[] = { "g_gitDownloadURL","g_gitDownloadURL2","g_gitDownloadURL3","g_gitDownloadURL4" };
				static const char *verKeys[] = { "g_gitVersion",    "g_gitVersion2",    "g_gitVersion3",    "g_gitVersion4"    };
				char urls[4][1024];
				char vers[4][128];
				int _s, _i;
				memset(urls, 0, sizeof(urls));
				memset(vers, 0, sizeof(vers));
				for (_i = 0; _i < uiInfo.serverStatusInfo.numLines; _i++) {
					if (!uiInfo.serverStatusInfo.lines[_i][0] || !uiInfo.serverStatusInfo.lines[_i][3]) continue;
					for (_s = 0; _s < 4; _s++) {
						if (!Q_stricmp(uiInfo.serverStatusInfo.lines[_i][0], urlKeys[_s]))
							Q_strncpyz(urls[_s], uiInfo.serverStatusInfo.lines[_i][3], sizeof(urls[_s]));
						if (!Q_stricmp(uiInfo.serverStatusInfo.lines[_i][0], verKeys[_s]))
							Q_strncpyz(vers[_s], uiInfo.serverStatusInfo.lines[_i][3], sizeof(vers[_s]));
					}
				}
				for (_s = 0; _s < 4; _s++) {
					if (!urls[_s][0]) continue;
					if (vers[_s][0])
						trap_Cmd_ExecuteText(EXEC_APPEND, va("cl_downloadserverassets \"%s\" \"%s\"\n", urls[_s], vers[_s]));
					else
						trap_Cmd_ExecuteText(EXEC_APPEND, va("cl_downloadserverassets \"%s\"\n", urls[_s]));
				}
			}
		} else if (Q_stricmp(name, "CheckServerAssets") == 0) { // 2C_ASSET_DOWNLOAD
			char info[MAX_STRING_CHARS];
			char assetUrl[1024];
			if (uiInfo.serverStatus.currentServer >= 0 &&
			    uiInfo.serverStatus.currentServer < uiInfo.serverStatus.numDisplayServers) {
				trap_LAN_GetServerInfo(ui_netSource.integer,
					uiInfo.serverStatus.displayServers[uiInfo.serverStatus.currentServer],
					info, MAX_STRING_CHARS);
				Q_strncpyz(assetUrl, Info_ValueForKey(info, "g_gitDownloadURL"), sizeof(assetUrl));
				trap_Cvar_Set("ui_serverHasAssets", assetUrl[0] ? "1" : "0");
			} else {
				trap_Cvar_Set("ui_serverHasAssets", "0");
			}

// ─── HUNK 2 — inside "ServerStatus" uiScript handler, after UI_BuildServerStatus(qtrue) ──

		trap_Cvar_Set("ui_serverHasAssets", "0"); // 2C_ASSET_DOWNLOAD: reset; set async in UI_BuildServerStatus

// ─── HUNK 3 — inside UI_BuildServerStatus(), after UI_GetServerStatusInfo() returns true ──

		// 2C_ASSET_DOWNLOAD: full getstatus response arrived — scan for any asset URL slot.
		{
			static const char *assetUrlKeys[] = { "g_gitDownloadURL","g_gitDownloadURL2","g_gitDownloadURL3","g_gitDownloadURL4" };
			qboolean hasAssets = qfalse;
			int _i, _s;
			for (_i = 0; _i < uiInfo.serverStatusInfo.numLines && !hasAssets; _i++) {
				if (!uiInfo.serverStatusInfo.lines[_i][0] || !uiInfo.serverStatusInfo.lines[_i][3]) continue;
				for (_s = 0; _s < 4; _s++) {
					if (!Q_stricmp(uiInfo.serverStatusInfo.lines[_i][0], assetUrlKeys[_s]) &&
					    uiInfo.serverStatusInfo.lines[_i][3][0]) {
						hasAssets = qtrue;
						break;
					}
				}
			}
			trap_Cvar_Set("ui_serverHasAssets", hasAssets ? "1" : "0");
		}

// ─── HUNK 4 — inside UI_JoinServer(), before the final connect call ──────────

		// 2C_ASSET_DOWNLOAD — block connect if a download is in progress; auto-connect when done.
		{
			char assetStat[64];
			trap_Cvar_VariableStringBuffer("cl_gitStatus", assetStat, sizeof(assetStat));
			qboolean dlBusy = (!Q_stricmp(assetStat, "checking")    ||
			                   !Q_stricmp(assetStat, "downloading")  ||
			                   !Q_stricmp(assetStat, "extracting"));
			if (dlBusy) {
				Com_Printf("^9[Assets] ^7Download in progress, will connect to %s when done.\n", buff);
				trap_Cvar_Set("cl_gitPendingConnect", buff);
				return;
			}
		}

		// 2C_ASSET_DOWNLOAD — auto-download on join when cl_gitAutoDownload is set.
		if (trap_Cvar_VariableValue("cl_gitAutoDownload") != 0) {
			static const char *urlKeys[] = { "g_gitDownloadURL","g_gitDownloadURL2","g_gitDownloadURL3","g_gitDownloadURL4" };
			static const char *verKeys[] = { "g_gitVersion",    "g_gitVersion2",    "g_gitVersion3",    "g_gitVersion4"    };
			char urls[4][1024];
			char vers[4][128];
			int _s, _i;
			qboolean anyQueued = qfalse;
			memset(urls, 0, sizeof(urls));
			memset(vers, 0, sizeof(vers));
			for (_i = 0; _i < uiInfo.serverStatusInfo.numLines; _i++) {
				if (!uiInfo.serverStatusInfo.lines[_i][0] || !uiInfo.serverStatusInfo.lines[_i][3]) continue;
				for (_s = 0; _s < 4; _s++) {
					if (!Q_stricmp(uiInfo.serverStatusInfo.lines[_i][0], urlKeys[_s]))
						Q_strncpyz(urls[_s], uiInfo.serverStatusInfo.lines[_i][3], sizeof(urls[_s]));
					if (!Q_stricmp(uiInfo.serverStatusInfo.lines[_i][0], verKeys[_s]))
						Q_strncpyz(vers[_s], uiInfo.serverStatusInfo.lines[_i][3], sizeof(vers[_s]));
				}
			}
			for (_s = 0; _s < 4; _s++) {
				if (!urls[_s][0]) continue;
				if (vers[_s][0])
					trap_Cmd_ExecuteText(EXEC_APPEND, va("cl_downloadserverassets \"%s\" \"%s\"\n", urls[_s], vers[_s]));
				else
					trap_Cmd_ExecuteText(EXEC_APPEND, va("cl_downloadserverassets \"%s\"\n", urls[_s]));
				anyQueued = qtrue;
			}
			if (anyQueued) {
				trap_Cvar_Set("cl_gitPendingConnect", buff);
				Com_Printf("^9[Assets] ^7Downloading server assets, will connect to %s when done.\n", buff);
				return;
			}
		}
