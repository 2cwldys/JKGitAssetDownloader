#pragma once

// 2C_ASSET_DOWNLOAD — server asset git release auto-download system
// Search for "2C_ASSET_DOWNLOAD" to find all related changes across files.
#define _2C_ASSET_DOWNLOAD

void CL_AssetDownload_Init(void);
void CL_AssetDownload_Frame(void);
void CL_AssetDownload_f(void);
void CL_AssetFlush_f(void);
qboolean CL_AssetDownload_IsBusy(void);
