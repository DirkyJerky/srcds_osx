/**
 * vim: set ts=4 :
 * =============================================================================
 * Source Dedicated Server Wrapper for Mac OS X
 * Copyright (C) 2011 Scott "DS" Ehlert.  All rights reserved.
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "hacks.h"
#include "mm_util.h"
#include "CDetour/detours.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

#include <AvailabilityMacros.h>
#include <CoreServices/CoreServices.h>
#include <mach/task.h>
#include <mach-o/nlist.h>

/* Define things from 10.6 SDK for older SDKs */
#ifndef MAC_OS_X_VERSION_10_6
#define TASK_DYLD_INFO 17
struct task_dyld_info
{
	mach_vm_address_t all_image_info_addr;
	mach_vm_size_t all_image_info_size;
};
typedef struct task_dyld_info task_dyld_info_data_t;
#define TASK_DYLD_INFO_COUNT (sizeof(task_dyld_info_data_t) / sizeof(natural_t))
#endif

static struct nlist dyld_syms[3];
static struct nlist steamclient_syms[4];
static struct nlist dedicated_syms[9];
static struct nlist launcher_syms[3];
static struct nlist engine_syms[2];

#if defined (ENGINE_L4D)
static struct nlist material_syms[2];
static struct nlist tier0_syms[2];
#endif

unsigned int g_AppId = 0;

void *g_Launcher = NULL;

CDetour *detSysLoadModules = NULL;
CDetour *detGetAppId = NULL;
CDetour *detSetAppId = NULL;

#if defined(ENGINE_OBV) || defined(ENGINE_L4D)
CDetour *detLoadModule = NULL;
CDetour *detSetShaderApi = NULL;
#endif

struct AppSystemInfo_t
{
	const char *m_pModuleName;
	const char *m_pInterfaceName;
};

typedef bool (*AddSystems_t)(void *, AppSystemInfo_t *);
AddSystems_t AppSysGroup_AddSystems = NULL;

template <typename T>
static inline T SymbolAddr(void *base, struct nlist *syms, size_t idx)
{
	return reinterpret_cast<T>(reinterpret_cast<uintptr_t>(base) + syms[idx].n_value);
}

bool InitSymbolData(const char *steamPath)
{
	memset(dyld_syms, 0, sizeof(dyld_syms));
	dyld_syms[0].n_un.n_name = (char *)"__ZL18_dyld_set_variablePKcS0_";
	dyld_syms[1].n_un.n_name = (char *)"_dyld_all_image_infos";

	if (nlist("/usr/lib/dyld", dyld_syms) != 0)
	{
		printf("Failed to find symbols for dyld\n");
		return false;
	}

#if defined(ENGINE_OBV)
	if (steamPath)
	{
		char clientPath[PATH_MAX];
		mm_Format(clientPath, sizeof(clientPath), "%s/steamclient.dylib", steamPath);
		
		memset(steamclient_syms, 0, sizeof(steamclient_syms));
		steamclient_syms[0].n_un.n_name = (char *)"__ZN14IClientUserMap14GetAccountNameEPcj";
		steamclient_syms[1].n_un.n_name = (char *)"__ZN15IClientUtilsMap8GetAppIDEv";
		steamclient_syms[2].n_un.n_name = (char *)"__ZN15IClientUtilsMap22SetAppIDForCurrentPipeEjb";
		
		if (nlist(clientPath, steamclient_syms) != 0)
		{
			printf("Failed to find symbols for steamclient.dylib\n");
			return false;
		}
	}
#endif

	memset(dedicated_syms, 0, sizeof(dedicated_syms));
	dedicated_syms[0].n_un.n_name = (char *)"__ZN4CSys11LoadModulesEP24CDedicatedAppSystemGroup";
	dedicated_syms[1].n_un.n_name = (char *)"__ZN15CAppSystemGroup10AddSystemsEP15AppSystemInfo_t";
	dedicated_syms[2].n_un.n_name = (char *)"_g_pFileSystem";
	dedicated_syms[3].n_un.n_name = (char *)"__ZL17g_pBaseFileSystem";
	dedicated_syms[4].n_un.n_name = (char *)"__Z14Sys_LoadModulePKc";
#if defined(ENGINE_OBV)
	dedicated_syms[5].n_un.n_name = steamPath ? (char *)"__ZL18g_FileSystem_Steam" : (char *)"_g_FileSystem_Stdio";
	dedicated_syms[6].n_un.n_name = (char *)"__ZN17CFileSystem_Steam4InitEv";
	dedicated_syms[7].n_un.n_name = (char *)"__Z17MountDependenciesiR10CUtlVectorIj10CUtlMemoryIjiEE";
#else
	dedicated_syms[5].n_un.n_name = (char *)"_g_FileSystem_Stdio";
#endif

	if (nlist("bin/dedicated.dylib", dedicated_syms) != 0)
	{
		printf("Failed to find symbols for dedicated.dylib\n");
		return false;
	}

	memset(launcher_syms, 0, sizeof(launcher_syms));
	launcher_syms[0].n_un.n_name = (char *)"__ZN15CAppSystemGroup9AddSystemEP10IAppSystemPKc";
	launcher_syms[1].n_un.n_name = (char *)"_g_CocoaMgr";

	if (nlist("bin/launcher.dylib", launcher_syms) != 0)
	{
		printf("Failed to find symbols for launcher.dylib\n");
		return false;
	}

	memset(engine_syms, 0, sizeof(engine_syms));
	engine_syms[0].n_un.n_name = (char *)"_g_pLauncherCocoaMgr";

	if (nlist("bin/engine.dylib", engine_syms) != 0)
	{
		printf("Failed to find symbols for engine.dylib\n");
		return false;
	}

#if defined(ENGINE_L4D)
	memset(material_syms, 0, sizeof(material_syms));
	material_syms[0].n_un.n_name = (char *)"__ZN15CMaterialSystem12SetShaderAPIEPKc";

	if (nlist("bin/materialsystem.dylib", material_syms) != 0)
	{
		printf("Failed to find symbols for materialsystem.dylib\n");
		return false;
	}
	
	memset(tier0_syms, 0, sizeof(tier0_syms));
	tier0_syms[0].n_un.n_name = (char *)"__Z12BuildCmdLineiPPc";

	if (nlist("bin/libtier0.dylib", tier0_syms) != 0)
	{
		printf("Failed to find symbols for libtier0.dylib\n");
		return false;
	}
#endif

	return true;
}

int SetLibraryPath(const char *path)
{	
	typedef void (*SetEnv_t)(const char *, const char *);
	int ret = setenv("DYLD_LIBRARY_PATH", path, 1);
	if (ret != 0)
	{
		return ret;
	}

	SInt32 osx_major, osx_minor;
	Gestalt(gestaltSystemVersionMajor, &osx_major);
	Gestalt(gestaltSystemVersionMinor, &osx_minor);

	if ((osx_major == 10 && osx_minor >= 6) || osx_major > 10)
	{
		task_dyld_info_data_t dyld_info;
		mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
		if (task_info(mach_task_self(), TASK_DYLD_INFO, (task_info_t)&dyld_info, &count) != KERN_SUCCESS)
		{
			printf("Failed to get dyld task info for current process\n");
			return -1;
		}
		/* Shift dyld address; this can happen with ASLR on Lion (10.7) */
		dyld_syms[0].n_value += int32_t(dyld_info.all_image_info_addr - dyld_syms[1].n_value);
	}
	
	/* A hacky hack */
	typedef void (*SetEnv_t)(const char *, const char *);
	SetEnv_t DyldSetEnv = SymbolAddr<SetEnv_t>(NULL, dyld_syms, 0);
	DyldSetEnv("DYLD_LIBRARY_PATH", path);
	
	return 0;
}

#if defined(ENGINE_L4D)

/*  Replace .so extension with .dylib */
static inline const char *FixLibraryExt(const char *pModuleName, char *buffer, size_t maxLength)
{
	size_t origLen = strlen(pModuleName);
	
	/*
	 * 3 extra chars are needed to do this.
	 *
	 * NOTE: 2nd condition is NOT >= due to null terminator.
	 */
	if (origLen > 3 && maxLength > origLen + 3)
	{
		size_t baseLen = origLen - 3;
		if (strncmp(pModuleName + baseLen, ".so", 3) == 0)
		{
			/* Yes, this should be safe now */
			memcpy(buffer, pModuleName, baseLen);
			strcpy(buffer + baseLen, ".dylib");
			
			return buffer;
		}
	}
	
	return pModuleName;
}

/* void CMaterialSystem::SetShaderAPI(const char *) */
DETOUR_DECL_MEMBER1(CMaterialSystem_SetShaderAPI, void, const char *, pModuleName)
{
	char module[PATH_MAX];

	pModuleName = FixLibraryExt(pModuleName, module, sizeof(module));

	DETOUR_MEMBER_CALL(CMaterialSystem_SetShaderAPI)(pModuleName);
	
	/* We can get rid of this now */
	detSetShaderApi->Destroy();
}

#endif

#if defined(ENGINE_OBV) || defined(ENGINE_L4D)

/* CSysModule *Sys_LoadModule(const char *) */
DETOUR_DECL_STATIC1(Sys_LoadModule, void *, const char *, pModuleName)
{
#if defined(ENGINE_OBV)
	if (strstr(pModuleName, "chromehtml.dylib"))
	{
		return NULL;
	}
	else
	{
		return DETOUR_STATIC_CALL(Sys_LoadModule)(pModuleName);
	}
#elif defined(ENGINE_L4D)
	char module[PATH_MAX];
	void *handle = NULL;

	pModuleName = FixLibraryExt(pModuleName, module, sizeof(module));
	
	handle = DETOUR_STATIC_CALL(Sys_LoadModule)(pModuleName);
	
	/* We need to install a detour in the materialsystem library, ugh */
	if (handle && strcmp(pModuleName, "bin/materialsystem.dylib") == 0)
	{
		Dl_info info;
		void *materialFactory = dlsym(handle, "CreateInterface");
		void *setShaderApi = NULL;

		if (!materialFactory)
		{
			printf("Failed to find CreateInterface (%s)\n", dlerror());
			return NULL;
		}
		
		if (!dladdr(materialFactory, &info) || !info.dli_fbase || !info.dli_fname)
		{
			printf("Failed to get base address of materialsystem.dylib\n");
			dlclose(handle);
			return NULL;
		}

		setShaderApi = SymbolAddr<void *>(info.dli_fbase, material_syms, 0);

		detSetShaderApi = DETOUR_CREATE_MEMBER(CMaterialSystem_SetShaderAPI, setShaderApi);
		if (!detSetShaderApi)
		{
			printf("Failed to create detour for CMaterialSystem::SetShaderAPI\n");
			return NULL;
		}

		detSetShaderApi->EnableDetour();

	}
	
	return handle;
#endif
}

#endif // ENGINE_L4D

#if defined(ENGINE_OBV)
DETOUR_DECL_MEMBER0(GetAppID, int)
{
	return g_AppId;
}

DETOUR_DECL_MEMBER2(SetAppIDForCurrentPipe, int, int, nAppID, bool, bTrackProcess)
{
	return DETOUR_MEMBER_CALL(SetAppIDForCurrentPipe)(g_AppId, bTrackProcess);
}
#endif

/* int CSys::LoadModules(CDedicatedAppSystemGroup *) */
DETOUR_DECL_MEMBER1(CSys_LoadModules, int, void *, appsys)
{
	Dl_info info;
	void *launcherMain;
	void *pCocoaMgr;
	void *engine;
	void *engineFactory;
	void **engineCocoa;
	int ret;

	typedef void (*AddSystem_t)(void *, void *, const char *);
	AddSystem_t AppSysGroup_AddSystem;

	g_Launcher = dlopen("launcher.dylib", RTLD_NOW);
	if (!g_Launcher)
	{
		printf("Failed to open launcher.dylib (%s)\n",  dlerror());
		return false;
	}
	
	launcherMain = dlsym(g_Launcher, "LauncherMain");
	if (!launcherMain)
	{
		printf("Failed to find launcher entry point (%s)\n", dlerror());
		dlclose(g_Launcher);
		g_Launcher = NULL;
		return false;
	}
	
	memset(&info, 0, sizeof(Dl_info)); 
	if (!dladdr(launcherMain, &info) || !info.dli_fbase || !info.dli_fname)
	{
		printf("Failed to get base address of launcher.dylib\n");
		dlclose(g_Launcher);
		g_Launcher = NULL;
		return false;
	}
	
	AppSysGroup_AddSystem = SymbolAddr<AddSystem_t>(info.dli_fbase, launcher_syms, 0);
	pCocoaMgr = SymbolAddr<void *>(info.dli_fbase, launcher_syms, 1);
	
	/* The engine and material system expect this interface to be available */
	AppSysGroup_AddSystem(appsys, pCocoaMgr, "CocoaMgrInterface006");

	AppSystemInfo_t sys_before[] =
	{
		{"inputsystem.dylib",	"InputSystemVersion001"},
		{"",					""}
	};
	AppSysGroup_AddSystems(appsys, sys_before);
	
	/* Call the original */
	ret = DETOUR_MEMBER_CALL(CSys_LoadModules)(appsys);
	
	/* Engine should already be loaded at this point by the original function */
	engine = dlopen("engine.dylib", RTLD_NOLOAD);
	if (!engine)
	{
		printf("Failed to get existing handle for engine.dylib (%s)\n", dlerror());
		return false;
	}
	
	engineFactory = dlsym(engine, "CreateInterface");
	if (!engineFactory)
	{
		printf("Failed to find CreateInterface (%s)\n", dlerror());
		dlclose(engine);
		return false;
	}
	
	if (!dladdr(engineFactory, &info) || !info.dli_fbase || !info.dli_fname)
	{
		printf("Failed to get base address of engine.dylib\n");
		dlclose(engine);
		return false;
	}
	
	engineCocoa = SymbolAddr<void **>(info.dli_fbase, engine_syms, 0);
	
	/* Prevent crash in engine function which expects this interface */
	*engineCocoa = pCocoaMgr;

	/* Load these to prevent crashes in engine and replay system */
	AppSystemInfo_t sys_after[] =
	{
#if defined(ENGINE_L4D2)
		{"vguimatsurface.dylib",	"VGUI_Surface031"},
#else
		{"vguimatsurface.dylib",	"VGUI_Surface030"},
#endif
		{"vgui2.dylib",				"VGUI_ivgui008"},
		{"",						""}
	};
	AppSysGroup_AddSystems(appsys, sys_after);

	dlclose(engine);

	return ret;
}

bool DoDedicatedHacks(void *entryPoint, bool steam, int appid)
{
	Dl_info info;
	void *sysLoad;
	void **pFileSystem;
	void **pBaseFileSystem;
	void *fileSystem;
#if defined(ENGINE_OBV) || defined(ENGINE_L4D)
	void *loadModule;
#endif

	memset(&info, 0, sizeof(Dl_info));
	if (!dladdr(entryPoint, &info) || !info.dli_fbase || !info.dli_fname)
	{
		printf("Failed to get base address of dedicated.dylib\n");
		return false;
	}
	
	sysLoad = SymbolAddr<unsigned char *>(info.dli_fbase, dedicated_syms, 0);
	AppSysGroup_AddSystems = SymbolAddr<AddSystems_t>(info.dli_fbase, dedicated_syms, 1);
	pFileSystem = SymbolAddr<void **>(info.dli_fbase, dedicated_syms, 2);
	pBaseFileSystem = SymbolAddr<void **>(info.dli_fbase, dedicated_syms, 3);
#if defined(ENGINE_OBV) || defined(ENGINE_L4D)
	loadModule = SymbolAddr<void *>(info.dli_fbase, dedicated_syms, 4);
#endif
	fileSystem = SymbolAddr<void *>(info.dli_fbase, dedicated_syms, 5);
	
	/* Work around conflicts between FileSystem_Stdio and FileSystem_Steam */
	*pFileSystem = fileSystem;
	*pBaseFileSystem = fileSystem;
	
#if defined(ENGINE_OBV)
	if (steam)
	{
		typedef int (*SteamInit_t)(void *);
		typedef void (*SteamMount_t)(int, void*);
		
		SteamInit_t SteamInit = SymbolAddr<SteamInit_t>(info.dli_fbase, dedicated_syms, 6);
		SteamMount_t SteamMount = SymbolAddr<SteamMount_t>(info.dli_fbase, dedicated_syms, 7);
		
		/* Init steam filesystem */
		SteamInit(fileSystem);

		char dummy[512];
		memset(dummy, 0, sizeof(dummy));
		
		/* Mount steam content */
		SteamMount(appid, dummy);
	}
#endif
	
	/* Detour CSys::LoadModules() */
	detSysLoadModules = DETOUR_CREATE_MEMBER(CSys_LoadModules, sysLoad);
	if (!detSysLoadModules)
	{
		printf("Failed to create detour for CSys::LoadModules\n");
		return false;
	}
	
#if defined(ENGINE_OBV) || defined(ENGINE_L4D)
	detLoadModule = DETOUR_CREATE_STATIC(Sys_LoadModule, loadModule);
	if (!detLoadModule)
	{
		printf("Failed to create detour for Sys_LoadModule\n");
		return false;
	}

	detLoadModule->EnableDetour();
#endif

	detSysLoadModules->EnableDetour();
	
	return true;
}

void RemoveDedicatedDetours()
{
	if (detSysLoadModules)
	{
		detSysLoadModules->Destroy();
	}

#if defined(ENGINE_OBV)
	if (detGetAppId)
	{
		detGetAppId->Destroy();
	}

	if (detSetAppId)
	{
		detSetAppId->Destroy();
	}
#endif
	
#if defined(ENGINE_L4D)
	if (detLoadModule)
	{
		detLoadModule->Destroy();
	}
#endif
}

#if defined(ENGINE_OBV)
bool ForceSteamAppId(unsigned int appid)
{
	void *steamclient;
	void *entryPoint;
	void *getAppId;
	void *setAppId;
	Dl_info info;

	steamclient = dlopen("steamclient.dylib", RTLD_LAZY);
	if (!steamclient)
	{
		printf("Failed to load steamclient.dylib\n");
		return false;
	}

	entryPoint = dlsym(steamclient, "CreateInterface");
	if (!entryPoint)
	{
		printf("Failed to get steamclient.dylib entry point\n");
		dlclose(steamclient);
		return false;
	}

	if (!dladdr(entryPoint, &info) || !info.dli_fbase || !info.dli_fname)
	{
		printf("Failed to get base address of steamclient.dylib\n");
		dlclose(steamclient);
		return false;
	}

	getAppId = SymbolAddr<void *>(info.dli_fbase, steamclient_syms, 1);
	setAppId = SymbolAddr<void *>(info.dli_fbase, steamclient_syms, 2);
	detGetAppId = DETOUR_CREATE_MEMBER(GetAppID, getAppId);
	detSetAppId = DETOUR_CREATE_MEMBER(SetAppIDForCurrentPipe, setAppId);

        if (!detGetAppId)
        {
                printf("Failed to detour GetAppID function\n");
		return false;
        }

	if (!detSetAppId)
	{
		printf("Failed to detour SetAppIDForCurrentPipe\n");
		return false;
	}

	detGetAppId->EnableDetour();
	detSetAppId->EnableDetour();
	g_AppId = appid;

	return true;
}
#endif

void *GetAccountNameFunc(const void *entryPoint)
{
	Dl_info info;
	if (!dladdr(entryPoint, &info) || !info.dli_fbase || !info.dli_fname)
	{
		printf("Failed to get base address of steamclient.dylib\n");
		return NULL;
	}

	return SymbolAddr<void *>(info.dli_fbase, steamclient_syms, 0);
}

#if defined(ENGINE_L4D)
void *GetBuildCmdLine()
{	
	Dl_info info;
	void *tier0 = NULL;
	void *msg = NULL;
	
	tier0 = dlopen("libtier0.dylib", RTLD_NOLOAD);
	if (!tier0)
	{
		printf("Failed to get existing handle for libtier0.dylib (%s)\n", dlerror());
		return NULL;
	}
	
	msg = dlsym(tier0, "Msg");
	if (!msg)
	{
		printf("Failed to find Msg (%s)\n", dlerror());
		dlclose(tier0);
		return NULL;
	}
	
	if (!dladdr(msg, &info) || !info.dli_fbase || !info.dli_fname)
	{
		printf("Failed to get base address of libtier0.dylib\n");
		dlclose(tier0);
		return NULL;
	}
	
	dlclose(tier0);
	
	return SymbolAddr<void *>(info.dli_fbase, tier0_syms, 0);
}
#endif

