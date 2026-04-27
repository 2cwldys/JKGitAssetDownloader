/*
 * JKGitAssetDownloader — cl_main.cpp additions
 *
 * 1. Add to includes near top of cl_main.cpp:
 *       #include "cl_assets.h"
 *
 * 2. Add to CL_Init() alongside other subsystem inits:
 *       CL_AssetDownload_Init();
 *
 * 3. Add to CL_Init() alongside other Cmd_AddCommand registrations:
 *       (the two commands below)
 *
 * 4. Add to CL_Frame() alongside other per-frame calls:
 *       CL_AssetDownload_Frame();
 */

#include "cl_assets.h" // 2C_ASSET_DOWNLOAD

// In CL_Init() — subsystem init
CL_AssetDownload_Init();

// In CL_Init() — command registration
Cmd_AddCommand("cl_downloadserverassets", CL_AssetDownload_f,  "Download and install server asset pack from URL"); // 2C_ASSET_DOWNLOAD
Cmd_AddCommand("cl_flushserverassets",    CL_AssetFlush_f,     "Clear cached asset version(s); next join will re-download"); // 2C_ASSET_DOWNLOAD

// In CL_Frame() — per-frame tick
CL_AssetDownload_Frame(); // 2C_ASSET_DOWNLOAD
