// ******************************************************************
// *
// *    .,-:::::    .,::      .::::::::.    .,::      .:
// *  ,;;;'````'    `;;;,  .,;;  ;;;'';;'   `;;;,  .,;;
// *  [[[             '[[,,[['   [[[__[[\.    '[[,,[['
// *  $$$              Y$$$P     $$""""Y$$     Y$$$P
// *  `88bo,__,o,    oP"``"Yo,  _88o,,od8P   oP"``"Yo,
// *    "YUMMMMMP",m"       "Mm,""YUMMMP" ,m"       "Mm,
// *
// *   Cxbx->Win32->CxbxKrnl->HLEIntercept.cpp
// *
// *  This file is part of the Cxbx project.
// *
// *  Cxbx and Cxbe are free software; you can redistribute them
// *  and/or modify them under the terms of the GNU General Public
// *  License as published by the Free Software Foundation; either
// *  version 2 of the license, or (at your option) any later version.
// *
// *  This program is distributed in the hope that it will be useful,
// *  but WITHOUT ANY WARRANTY; without even the implied warranty of
// *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// *  GNU General Public License for more details.
// *
// *  You should have recieved a copy of the GNU General Public License
// *  along with this program; see the file COPYING.
// *  If not, write to the Free Software Foundation, Inc.,
// *  59 Temple Place - Suite 330, Bostom, MA 02111-1307, USA.
// *
// *  (c) 2002-2003 Aaron Robinson <caustik@caustik.com>
// *
// *  All rights reserved
// *
// ******************************************************************
#define _CXBXKRNL_INTERNAL

#include <cmath>
#include "CxbxKrnl.h"
#include "Emu.h"
#include "EmuFS.h"
#include "EmuXTL.h"
#include "EmuShared.h"
#include "HLEDataBase.h"
#include "HLEIntercept.h"

static void *EmuLocateFunction(OOVPA *Oovpa, uint32 lower, uint32 upper);
static void  EmuInstallWrappers(OOVPATable *OovpaTable, uint32 OovpaTableSize, Xbe::Header *pXbeHeader);
static void  EmuXRefFailure();

#include <shlobj.h>
#include <vector>

uint32 fcount = 0;
uint32 funcExclude[2048] = {0};

uint32 g_BuildVersion;
uint32 g_OrigBuildVersion;

static std::vector<void *> vCacheOut;

static bool bCacheInp = false;
static std::vector<void *> vCacheInp;
static std::vector<void*>::const_iterator vCacheInpIter;

void EmuHLEIntercept(Xbe::LibraryVersion *pLibraryVersion, Xbe::Header *pXbeHeader)
{
    Xbe::Certificate *pCertificate = (Xbe::Certificate*)pXbeHeader->dwCertificateAddr;

    char szCacheFileName[260];

    DbgPrintf("\n");
    DbgPrintf("*******************************************************************************\n");
    DbgPrintf("* Cxbx High Level Emulation database last modified %s\n", szHLELastCompileTime);
    DbgPrintf("*******************************************************************************\n");
    DbgPrintf("\n");

    //
    // initialize HLE cache file
    //

    {
        SHGetSpecialFolderPath(NULL, szCacheFileName, CSIDL_APPDATA, TRUE);

        strcat(szCacheFileName, "\\Cxbx-Reloaded\\");

        CreateDirectory(szCacheFileName, NULL);

        sint32 spot = -1;

        for(int v=0;v<260;v++)
        {
            if(szCacheFileName[v] == '\\') { spot = v; }
            else if(szCacheFileName[v] == '\0') { break; }
        }

        if(spot != -1) { szCacheFileName[spot] = '\0'; }

        //
        // create HLECache directory
        //

        strcpy(&szCacheFileName[spot], "\\HLECache");

        CreateDirectory(szCacheFileName, NULL);

        //
        // open title's cache file
        //

        sprintf(&szCacheFileName[spot+9], "\\%08x.dat", pCertificate->dwTitleId);

        FILE *pCacheFile = fopen(szCacheFileName, "rb");

        if(pCacheFile != NULL)
        {
            bool bVerified = false;

            //
            // verify last compiled timestamp
            //

            char szCacheLastCompileTime[64];

            memset(szCacheLastCompileTime, 0, 64);

            if(fread(szCacheLastCompileTime, 64, 1, pCacheFile) == 1)
            {
                if(strcmp(szCacheLastCompileTime, szHLELastCompileTime) == 0)
                {
                    bVerified = true;
                }
            }

            //
            // load function addresses
            //

            if(bVerified)
            {
                while(true)
                {
                    void *cur;

                    if(fread(&cur, 4, 1, pCacheFile) != 1)
                        break;

                    vCacheInp.push_back(cur);
                }

                bCacheInp = true;

                vCacheInpIter = vCacheInp.begin();

                DbgPrintf("HLE: Loaded HLE Cache for 0x%.08X\n", pCertificate->dwTitleId);
            }

            fclose(pCacheFile);
        }
    }

    //
    // initialize openxdk emulation (TODO)
    //

    if(pLibraryVersion == 0)
    {
        DbgPrintf("HLE: Detected OpenXDK application...\n");
    }

    //
    // initialize Microsoft XDK emulation
    //

    if(pLibraryVersion != 0)
    {
        DbgPrintf("HLE: Detected Microsoft XDK application...\n");

        uint32 dwLibraryVersions = pXbeHeader->dwLibraryVersions;
        uint32 dwHLEEntries = HLEDataBaseSize / sizeof(HLEData);

        uint32 LastUnResolvedXRefs = UnResolvedXRefs+1;
        uint32 OrigUnResolvedXRefs = UnResolvedXRefs;

        for(int p=0;UnResolvedXRefs < LastUnResolvedXRefs;p++)
        {
            DbgPrintf("HLE: Starting pass #%d...\n", p+1);

            LastUnResolvedXRefs = UnResolvedXRefs;

            bool bFoundD3D = false;
            for(uint32 v=0;v<dwLibraryVersions;v++)
            {
                uint16 MajorVersion = pLibraryVersion[v].wMajorVersion;
                uint16 MinorVersion = pLibraryVersion[v].wMinorVersion;
                uint16 BuildVersion = pLibraryVersion[v].wBuildVersion;
                uint16 OrigBuildVersion = BuildVersion;

                // Aliases - for testing purposes only
				if(BuildVersion == 4039) { BuildVersion = 4034; }
				if(BuildVersion == 4238) { BuildVersion = 4361; }	// I don't think this XDK was released.
				if(BuildVersion == 4242) { BuildVersion = 4361; }
				if(BuildVersion == 4400) { BuildVersion = 4361; }
				if(BuildVersion == 4531) { BuildVersion = 4432; }
                if(BuildVersion == 4721) { BuildVersion = 4627; }
				if(BuildVersion == 4831) { BuildVersion = 4627; }
                if(BuildVersion == 4928) { BuildVersion = 4627; }
                if(BuildVersion == 5344) { BuildVersion = 5233; }
                if(BuildVersion == 5455) { BuildVersion = 5558; }
                if(BuildVersion == 5659) { BuildVersion = 5558; }
                if(BuildVersion == 5788) { BuildVersion = 5558; }
                if(BuildVersion == 5028) { BuildVersion = 4627; }
				if(BuildVersion == 5120) { BuildVersion = 5233; }
                if(BuildVersion == 5933) { BuildVersion = 5849; }   // These XDK versions are pretty much the same
                /*
                if(BuildVersion == 3944) { BuildVersion = 3925; }
                if(BuildVersion == 4039) { BuildVersion = 4034; }
                if(BuildVersion == 4242) { BuildVersion = 4432; }
                if(BuildVersion == 4531) { BuildVersion = 4432; }
                if(BuildVersion == 4721) { BuildVersion = 4432; }
                if(BuildVersion == 4831) { BuildVersion = 4432; }
                if(BuildVersion == 4928) { BuildVersion = 4432; }
                if(BuildVersion == 5028) { BuildVersion = 4432; }
                if(BuildVersion == 5120) { BuildVersion = 4432; }
                if(BuildVersion == 5344) { BuildVersion = 4432; }
                if(BuildVersion == 5455) { BuildVersion = 4432; }
                if(BuildVersion == 5933) { BuildVersion = 4432; }
                */

                char szLibraryName[9] = {0};
				char szOrigLibraryName[9] = {0};

                for(uint32 c=0;c<8;c++)
                {
                    szLibraryName[c] = pLibraryVersion[v].szName[c];
					szOrigLibraryName[c] = pLibraryVersion[v].szName[c];
                }

				// Test
				if(strcmp(szLibraryName, "XGRAPHC") == 0)
				{
				//	if(BuildVersion == 4432)
				//		BuildVersion = 4361;
					if(BuildVersion == 3944)
						BuildVersion = 3911;
					if(OrigBuildVersion == 4531)
						BuildVersion = 4361;
					// Quick test (JSRF)
					if(OrigBuildVersion == 4134)
						BuildVersion = 4361;
					// Quick test (Simpsons: RoadRage)
				//	if(BuildVersion == 4034)
				//		BuildVersion = 3911;
				}

                // Several 3911 titles has different DSound builds.
                if(strcmp(szLibraryName, "DSOUND") == 0)
                {
                    if(BuildVersion < 4034)
                    {
                        BuildVersion = 3936;
                    }

					// Redirect other highly similar DSOUND library versions
					if(BuildVersion == 4361 || BuildVersion == 4400 || BuildVersion == 4432 || 
						BuildVersion == 4531 )
						BuildVersion = 4627;
                }

				// Change a few XAPILIB versions to similar counterparts
				if(strcmp(szLibraryName, "XAPILIB") == 0)
				{
					if(BuildVersion == 3944)
						BuildVersion = 3911;
					if(BuildVersion == 3950)
						BuildVersion = 3911;
					if(OrigBuildVersion == 4531)
						BuildVersion = 4627;
				}

                if(bXRefFirstPass)
                {
                    if(strcmp("XAPILIB", szLibraryName) == 0 && MajorVersion == 1 && MinorVersion == 0 &&
                        (BuildVersion == 3911 || BuildVersion == 4034 || BuildVersion == 4134 || BuildVersion == 4361
                      || BuildVersion == 4432 || BuildVersion == 4627 || BuildVersion == 5233 || BuildVersion == 5558
                      || BuildVersion == 5849))
                    {
                        uint32 lower = pXbeHeader->dwBaseAddr;
                        uint32 upper = pXbeHeader->dwBaseAddr + pXbeHeader->dwSizeofImage;
                    }
                }

                DbgPrintf("HLE: * Searching HLE database for %s %d.%d.%d...", szLibraryName, MajorVersion, MinorVersion, BuildVersion);

                bool found=false;

                for(uint32 d=0;d<dwHLEEntries;d++)
                {
                    if(BuildVersion != HLEDataBase[d].BuildVersion || MinorVersion != HLEDataBase[d].MinorVersion || MajorVersion != HLEDataBase[d].MajorVersion || strcmp(szLibraryName, HLEDataBase[d].Library) != 0)
                        continue;

                    found = true;

                    DbgPrintf("Found\n");

                    EmuInstallWrappers(HLEDataBase[d].OovpaTable, HLEDataBase[d].OovpaTableSize, pXbeHeader);
                }

                if(!found) { DbgPrintf("Skipped\n"); }
            }

            bXRefFirstPass = false;
        }

        // display Xref summary
        DbgPrintf("HLE: Resolved %d cross reference(s)\n", OrigUnResolvedXRefs - UnResolvedXRefs);
    }

    vCacheInp.clear();

    //
    // update cache file
    //
/* Turn of the nasty HLE cacheing (When you are adding oovaps anyway), it's in dire need of a better file identify system
    if(vCacheOut.size() > 0)
    {
        FILE *pCacheFile = fopen(szCacheFileName, "wb");

        if(pCacheFile != NULL)
        {
            DbgPrintf("HLE: Saving HLE Cache for 0x%.08X...\n", pCertificate->dwTitleId);

            //
            // write last compiled timestamp
            //

            char szCacheLastCompileTime[64];

            memset(szCacheLastCompileTime, 0, 64);

            strcpy(szCacheLastCompileTime, szHLELastCompileTime);

            fwrite(szCacheLastCompileTime, 64, 1, pCacheFile);

            //
            // write function addresses
            //

            std::vector<void*>::const_iterator cur;

            for(cur = vCacheOut.begin();cur != vCacheOut.end(); ++cur)
            {
                fwrite(&(*cur), 4, 1, pCacheFile);
            }
        }

        fclose(pCacheFile);
    }
*/
    vCacheOut.clear();

    DbgPrintf("\n");

    return;
}

// install function interception wrapper
static inline void EmuInstallWrapper(void *FunctionAddr, void *WrapperAddr)
{
    uint08 *FuncBytes = (uint08*)FunctionAddr;

    *(uint08*)&FuncBytes[0] = 0xE9;
    *(uint32*)&FuncBytes[1] = (uint32)WrapperAddr - (uint32)FunctionAddr - 5;
}

// locate the given function, searching within lower and upper bounds
static void *EmuLocateFunction(OOVPA *Oovpa, uint32 lower, uint32 upper)
{
    uint32 count = Oovpa->Count;

    // Skip out if this is an unnecessary search
    if(!bXRefFirstPass && Oovpa->XRefCount == 0 && Oovpa->XRefSaveIndex == XRefNotSaved)
        return 0;

    // large
    if(Oovpa->Large == 1)
    {
        LOOVPA<1> *Loovpa = (LOOVPA<1>*)Oovpa;

        upper -= Loovpa->Lovp[count-1].Offset;

        // search all of the image memory
        for(uint32 cur=lower;cur<upper;cur++)
        {
            uint32 v;

            // check all cross references
            for(v=0;v<Loovpa->XRefCount;v++)
            {
                uint32 Offset = Loovpa->Lovp[v].Offset;
                uint32 Value  = Loovpa->Lovp[v].Value;

                uint32 RealValue = *(uint32*)(cur + Offset);

                if(XRefDataBase[Value] == -1)
                    goto skipout_L;   // unsatisfied Xref is not acceptable

                if((RealValue + cur + Offset+4 != XRefDataBase[Value]) && (RealValue != XRefDataBase[Value]))
                    break;
            }

            // check all pairs, moving on if any do not match
            for(v=0;v<count;v++)
            {
                uint32 Offset = Loovpa->Lovp[v].Offset;
                uint32 Value  = Loovpa->Lovp[v].Value;

                uint08 RealValue = *(uint08*)(cur + Offset);

                if(RealValue != Value)
                    break;
            }

            // success if we found all pairs
            if(v == count)
            {
                if(Loovpa->XRefSaveIndex != XRefNotSaved)
                {
                    if(XRefDataBase[Loovpa->XRefSaveIndex] == -1)
                    {
                        UnResolvedXRefs--;
                        XRefDataBase[Loovpa->XRefSaveIndex] = cur;

                        return (void*)cur;
                    }
                    else
                    {
                        return (void*)XRefDataBase[Loovpa->XRefSaveIndex];   // already Found, no bother patching again
                    }
                }

                return (void*)cur;
            }

            skipout_L:;
        }
    }
    // small
    else
    {
        SOOVPA<1> *Soovpa = (SOOVPA<1>*)Oovpa;

        upper -= Soovpa->Sovp[count-1].Offset;

        // search all of the image memory
        for(uint32 cur=lower;cur<upper;cur++)
        {
            uint32 v;

            // check all cross references
            for(v=0;v<Soovpa->XRefCount;v++)
            {
                uint32 Offset = Soovpa->Sovp[v].Offset;
                uint32 Value  = Soovpa->Sovp[v].Value;

                uint32 RealValue = *(uint32*)(cur + Offset);

                if(XRefDataBase[Value] == -1)
                    goto skipout_S;   // Unsatisfied XRef is not acceptable

                if( (RealValue + cur + Offset + 4 != XRefDataBase[Value]) && (RealValue != XRefDataBase[Value]))
                    break;
            }

            // check OV pairs if all xrefs matched
            if(v == Soovpa->XRefCount)
            {
                // check all pairs, moving on if any do not match
                for(;v<count;v++)
                {
                    uint32 Offset = Soovpa->Sovp[v].Offset;
                    uint32 Value  = Soovpa->Sovp[v].Value;

                    uint08 RealValue = *(uint08*)(cur + Offset);

                    if(RealValue != Value)
                        break;
                }
            }

            // success if we found all pairs
            if(v == count)
            {
                if(Soovpa->XRefSaveIndex != XRefNotSaved)
                {
                    if(XRefDataBase[Soovpa->XRefSaveIndex] == -1)
                    {
                        UnResolvedXRefs--;
                        XRefDataBase[Soovpa->XRefSaveIndex] = cur;

                        return (void*)cur;
                    }
                    else
                    {
                        return (void*)XRefDataBase[Soovpa->XRefSaveIndex];   // already Found, no bother patching again
                    }
                }

                return (void*)cur;
            }

            skipout_S:;
        }
    }

    return 0;
}

// install function interception wrappers
static void EmuInstallWrappers(OOVPATable *OovpaTable, uint32 OovpaTableSize, Xbe::Header *pXbeHeader)
{
    uint32 lower = pXbeHeader->dwBaseAddr;
    uint32 upper = pXbeHeader->dwBaseAddr + pXbeHeader->dwSizeofImage;

    // traverse the full OOVPA table
    for(uint32 a=0;a<OovpaTableSize/sizeof(OOVPATable);a++)
    {
        OOVPA *Oovpa = OovpaTable[a].Oovpa;

        void *pFunc = NULL;

        if(bCacheInp && (vCacheInpIter != vCacheInp.end()))
        {
            pFunc = (*vCacheInpIter);

            ++vCacheInpIter;
        }
        else
        {
            pFunc = EmuLocateFunction(Oovpa, lower, upper);
            vCacheOut.push_back(pFunc);
        }

        if(pFunc != 0)
        {
            #ifdef _DEBUG_TRACE
            DbgPrintf("HLE: 0x%.08X -> %s\n", pFunc, OovpaTable[a].szFuncName);
            #endif

            if(OovpaTable[a].lpRedirect == 0)
            {
                EmuInstallWrapper(pFunc, EmuXRefFailure);
            }
            else
            {
                EmuInstallWrapper(pFunc, OovpaTable[a].lpRedirect);
                funcExclude[fcount++] = (uint32)pFunc;
            }
        }
    }
}

// alert for the situation where an Xref function body is hit
static void EmuXRefFailure()
{
    _asm int 3;
    CxbxKrnlCleanup("XRef-only function body reached. Fatal Error.");
}

