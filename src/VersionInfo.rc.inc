// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 RadegasTCZ
// Shared VERSIONINFO for every binary we ship (Explorer > Properties > Details). Include it from a target's .rc
// after defining VI_DESCRIPTION, VI_FILENAME and VI_FILETYPE (VFT_DLL / VFT_APP).
//
// The numbers come from version.h in the build's "generated" folder. CMake writes a development one
// (x.y.0.0) at configure time; scripts\Build-Package.ps1 writes the release one (the same version as the
// driver packages' DriverVer) and rebuilds the binaries before packaging them.
#include <winver.h>
#include "version.h"

VS_VERSION_INFO VERSIONINFO
 FILEVERSION     WAE_VERSION_BIN
 PRODUCTVERSION  WAE_VERSION_BIN
 FILEFLAGSMASK   VS_FFI_FILEFLAGSMASK
 FILEFLAGS       WAE_VERSION_FLAGS
 FILEOS          VOS_NT_WINDOWS32
 FILETYPE        VI_FILETYPE
 FILESUBTYPE     VFT2_UNKNOWN
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"   // English (US), Unicode
        BEGIN
            VALUE "CompanyName",      "RadegasTCZ"
            VALUE "FileDescription",  VI_DESCRIPTION
            VALUE "FileVersion",      WAE_VERSION_STR
            VALUE "InternalName",     VI_FILENAME
            VALUE "LegalCopyright",   "Copyright (C) 2026 RadegasTCZ. Licensed under the GNU GPL v3 or later."
            VALUE "OriginalFilename", VI_FILENAME
            VALUE "ProductName",      "WebRTC Audio Engine"
            VALUE "ProductVersion",   WAE_VERSION_STR
            VALUE "Comments",         "https://github.com/RadegasTCZ/WebRTC-Audio-Engine"
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
