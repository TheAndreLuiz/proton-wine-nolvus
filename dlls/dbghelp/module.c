/*
 * File module.c - module handling for the wine debugger
 *
 * Copyright (C) 1993,      Eric Youngdale.
 * 		 2000-2004, Eric Pouech
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "dbghelp_private.h"
#include "psapi.h"
#include "winreg.h"
#include "winternl.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dbghelp);

/***********************************************************************
 * Creates and links a new module to a process 
 */
struct module* module_new(struct process* pcs, const char* name, 
                          enum DbgModuleType type, 
                          unsigned long mod_addr, unsigned long size,
                          unsigned long stamp, unsigned long checksum) 
{
    struct module*      module;
    const char*         ptr;

    if (!(module = HeapAlloc(GetProcessHeap(), 0, sizeof(*module))))
	return NULL;

    memset(module, 0, sizeof(*module));

    module->next = pcs->lmodules;
    pcs->lmodules = module;

    TRACE("=> %s %08lx-%08lx %s\n", 
          type == DMT_ELF ? "ELF" : (type == DMT_PE ? "PE" : "---"),
          mod_addr, mod_addr + size, name);

    pool_init(&module->pool, 65536);
    
    module->module.SizeOfStruct = sizeof(module->module);
    module->module.BaseOfImage = mod_addr;
    module->module.ImageSize = size;
    for (ptr = name + strlen(name) - 1; 
         *ptr != '/' && *ptr != '\\' && ptr >= name; 
         ptr--);
    if (ptr < name || *ptr == '/' || *ptr == '\\') ptr++;
    strncpy(module->module.ModuleName, ptr, sizeof(module->module.ModuleName));
    module->module.ModuleName[sizeof(module->module.ModuleName) - 1] = '\0';
    module->module.ImageName[0] = '\0';
    strncpy(module->module.LoadedImageName, name, 
            sizeof(module->module.LoadedImageName));
    module->module.LoadedImageName[sizeof(module->module.LoadedImageName) - 1] = '\0';
    module->module.SymType = SymNone;
    module->module.NumSyms = 0;
    module->module.TimeDateStamp = stamp;
    module->module.CheckSum = checksum;

    module->type              = type;
    module->sortlist_valid    = FALSE;
    module->addr_sorttab      = NULL;
    /* FIXME: this seems a bit too high (on a per module basis)
     * need some statistics about this
     */
    hash_table_init(&module->pool, &module->ht_symbols, 4096);
    hash_table_init(&module->pool, &module->ht_types,   4096);

    module->sources_used      = 0;
    module->sources_alloc     = 0;
    module->sources           = 0;

    return module;
}

/***********************************************************************
 *	module_find_by_name
 *
 */
struct module* module_find_by_name(const struct process* pcs, 
                                   const char* name, enum DbgModuleType type)
{
    struct module*      module;

    if (type == DMT_UNKNOWN)
    {
        if ((module = module_find_by_name(pcs, name, DMT_PE)) ||
            (module = module_find_by_name(pcs, name, DMT_ELF)))
            return module;
    }
    else
    {
        for (module = pcs->lmodules; module; module = module->next)
        {
            if (type == module->type && !strcasecmp(name, module->module.LoadedImageName)) 
                return module;
        }
        for (module = pcs->lmodules; module; module = module->next)
        {
            if (type == module->type && !strcasecmp(name, module->module.ModuleName)) 
                return module;
        }
    }
    SetLastError(ERROR_INVALID_NAME);
    return NULL;
}

/***********************************************************************
 *           module_has_container
 *
 */
static struct module* module_get_container(const struct process* pcs, 
                                           const struct module* inner)
{
    struct module*      module;
     
    for (module = pcs->lmodules; module; module = module->next)
    {
        if (module != inner &&
            module->module.BaseOfImage <= inner->module.BaseOfImage &&
            module->module.BaseOfImage + module->module.ImageSize >=
            inner->module.BaseOfImage + inner->module.ImageSize)
            return module;
    }
    return NULL;
}

/******************************************************************
 *		module_get_debug
 *
 * get the debug information from a module:
 * - if the module's type is deferred, then force loading of debug info (and return
 *   the module itself)
 * - if the module has no debug info and has an ELF container, then return the ELF
 *   container (and also force the ELF container's debug info loading if deferred)
 * - otherwise return the module itself if it has some debug info
 */
struct module* module_get_debug(const struct process* pcs, struct module* module)
{
    if (!module) return NULL;
    switch (module->module.SymType)
    {
    case -1: break;
    case SymNone:
        module = module_get_container(pcs, module);
        if (!module || module->module.SymType != SymDeferred) break;
        /* fall through */
    case SymDeferred:
        switch (module->type)
        {
        case DMT_ELF:
            elf_load_debug_info(module);
            break;
        case DMT_PE:
            pe_load_debug_info(pcs, module);
            break;
        default: break;
        }
        break;
    default: break;
    }
    return (module && module->module.SymType > SymNone) ? module : NULL;
}

/***********************************************************************
 *	module_find_by_addr
 *
 * either the addr where module is loaded, or any address inside the 
 * module
 */
struct module* module_find_by_addr(const struct process* pcs, unsigned long addr, 
                                   enum DbgModuleType type)
{
    struct module*      module;
    
    if (type == DMT_UNKNOWN)
    {
        if ((module = module_find_by_addr(pcs, addr, DMT_PE)) ||
            (module = module_find_by_addr(pcs, addr, DMT_ELF)))
            return module;
    }
    else
    {
        for (module = pcs->lmodules; module; module = module->next)
        {
            if (type == module->type && addr >= module->module.BaseOfImage &&
                addr < module->module.BaseOfImage + module->module.ImageSize) 
                return module;
        }
    }
    SetLastError(ERROR_INVALID_ADDRESS);
    return module;
}

/***********************************************************************
 *			SymLoadModule (DBGHELP.@)
 */
DWORD WINAPI SymLoadModule(HANDLE hProcess, HANDLE hFile, char* ImageName,
                           char* ModuleName, DWORD BaseOfDll, DWORD SizeOfDll)
{
    struct process*     pcs;
    struct module*	module = NULL;

    TRACE("(%p %p %s %s %08lx %08lx)\n", 
          hProcess, hFile, debugstr_a(ImageName), debugstr_a(ModuleName), 
          BaseOfDll, SizeOfDll);

    pcs = process_find_by_handle(hProcess);
    if (!pcs) return FALSE;

    if (!(module = pe_load_module(pcs, ImageName, hFile, BaseOfDll, SizeOfDll)))
    {
        unsigned        len = strlen(ImageName);

        if (!strcmp(ImageName + len - 3, ".so") &&
            (module = elf_load_module(pcs, ImageName))) goto done;
        if ((module = pe_load_module_from_pcs(pcs, ImageName, ModuleName, BaseOfDll, SizeOfDll)))
            goto done;
        WARN("Couldn't locate %s\n", ImageName);
        return 0;
    }

done:
    /* by default pe_load_module fills module.ModuleName from a derivation 
     * of ImageName. Overwrite it, if we have better information
     */
    if (ModuleName)
    {
        strncpy(module->module.ModuleName, ModuleName, 
                sizeof(module->module.ModuleName));
        module->module.ModuleName[sizeof(module->module.ModuleName) - 1] = '\0';
    }
    strncpy(module->module.ImageName, ImageName, sizeof(module->module.ImageName));
    module->module.ImageName[sizeof(module->module.ImageName) - 1] = '\0';
    /* force transparent ELF loading / unloading */
    if (module->type != DMT_ELF) elf_synchronize_module_list(pcs);

    return module->module.BaseOfImage;
}

/******************************************************************
 *		module_remove
 *
 */
BOOL module_remove(struct process* pcs, struct module* module)
{
    struct module**     p;

    TRACE("%s (%p)\n", module->module.ModuleName, module);
    hash_table_destroy(&module->ht_symbols);
    hash_table_destroy(&module->ht_types);
    HeapFree(GetProcessHeap(), 0, (char*)module->sources);
    HeapFree(GetProcessHeap(), 0, module->addr_sorttab);
    pool_destroy(&module->pool);

    for (p = &pcs->lmodules; *p; p = &(*p)->next)
    {
        if (*p == module)
        {
            *p = module->next;
            HeapFree(GetProcessHeap(), 0, module);
            return TRUE;
        }
    }
    FIXME("This shouldn't happen\n");
    return FALSE;
}

/******************************************************************
 *		SymUnloadModule (DBGHELP.@)
 *
 */
BOOL WINAPI SymUnloadModule(HANDLE hProcess, DWORD BaseOfDll)
{
    struct process*     pcs;
    struct module*      module;

    pcs = process_find_by_handle(hProcess);
    if (!pcs) return FALSE;
    module = module_find_by_addr(pcs, BaseOfDll, DMT_UNKNOWN);
    if (!module) return FALSE;
    return module_remove(pcs, module);
}

/******************************************************************
 *		SymEnumerateModules (DBGHELP.@)
 *
 */
BOOL  WINAPI SymEnumerateModules(HANDLE hProcess,
                                 PSYM_ENUMMODULES_CALLBACK EnumModulesCallback,  
                                 PVOID UserContext)
{
    struct process*     pcs = process_find_by_handle(hProcess);
    struct module*      module;

    if (!pcs) return FALSE;
    
    for (module = pcs->lmodules; module; module = module->next)
    {
        if (module->type != DMT_PE) continue;
        if (!EnumModulesCallback(module->module.ModuleName, 
                                 module->module.BaseOfImage, UserContext))
            break;
    }
    return TRUE;
}

/******************************************************************
 *		EnumerateLoadedModules (DBGHELP.@)
 *
 */
BOOL  WINAPI EnumerateLoadedModules(HANDLE hProcess,
                                    PENUMLOADED_MODULES_CALLBACK EnumLoadedModulesCallback,
                                    PVOID UserContext)
{
    HMODULE*    hMods;
    char        img[256], mod[256];
    DWORD       i, sz;
    MODULEINFO  mi;

    hMods = HeapAlloc(GetProcessHeap(), 0, sz);
    if (!hMods) return FALSE;

    if (!EnumProcessModules(hProcess, hMods, 256 * sizeof(hMods[0]), &sz))
    {
        /* hProcess should also be a valid process handle !! */
        FIXME("If this happens, bump the number in mod\n");
        HeapFree(GetProcessHeap(), 0, hMods);
        return FALSE;
    }
    sz /= sizeof(HMODULE);
    for (i = 0; i < sz; i++)
    {
        if (!GetModuleInformation(hProcess, hMods[i], &mi, sizeof(mi)) ||
            !GetModuleFileNameExA(hProcess, hMods[i], img, sizeof(img)) ||
            !GetModuleBaseNameA(hProcess, hMods[i], mod, sizeof(mod)))
            break;
        EnumLoadedModulesCallback(mod, (DWORD)mi.lpBaseOfDll, mi.SizeOfImage, 
                                  UserContext);
    }
    HeapFree(GetProcessHeap(), 0, hMods);

    return sz != 0 && i == sz;
}

/******************************************************************
 *		SymGetModuleInfo (DBGHELP.@)
 *
 */
BOOL  WINAPI SymGetModuleInfo(HANDLE hProcess, DWORD dwAddr, 
                              PIMAGEHLP_MODULE ModuleInfo)
{
    struct process*     pcs = process_find_by_handle(hProcess);
    struct module*      module;

    if (!pcs) return FALSE;
    if (ModuleInfo->SizeOfStruct < sizeof(*ModuleInfo)) return FALSE;
    module = module_find_by_addr(pcs, dwAddr, DMT_UNKNOWN);
    if (!module) return FALSE;

    *ModuleInfo = module->module;
    if (module->module.SymType <= SymNone)
    {
        module = module_get_container(pcs, module);
        if (module && module->module.SymType > SymNone)
            ModuleInfo->SymType = module->module.SymType;
    }

    return TRUE;
}

/***********************************************************************
 *		SymGetModuleBase (IMAGEHLP.@)
 */
DWORD WINAPI SymGetModuleBase(HANDLE hProcess, DWORD dwAddr)
{
    struct process*     pcs = process_find_by_handle(hProcess);
    struct module*      module;

    if (!pcs) return 0;
    module = module_find_by_addr(pcs, dwAddr, DMT_UNKNOWN);
    if (!module) return 0;
    return module->module.BaseOfImage;
}
