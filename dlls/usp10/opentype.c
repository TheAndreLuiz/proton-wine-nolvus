/*
 * Opentype font interfaces for the Uniscribe Script Processor (usp10.dll)
 *
 * Copyright 2012 CodeWeavers, Aric Stewart
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
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 */
#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winnls.h"
#include "usp10.h"
#include "winternl.h"

#include "usp10_internal.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(uniscribe);

#ifdef WORDS_BIGENDIAN
#define GET_BE_WORD(x) (x)
#define GET_BE_DWORD(x) (x)
#else
#define GET_BE_WORD(x) RtlUshortByteSwap(x)
#define GET_BE_DWORD(x) RtlUlongByteSwap(x)
#endif

/* These are all structures needed for the cmap format 12 table */
#define CMAP_TAG MS_MAKE_TAG('c', 'm', 'a', 'p')

typedef struct {
    WORD platformID;
    WORD encodingID;
    DWORD offset;
} CMAP_EncodingRecord;

typedef struct {
    WORD version;
    WORD numTables;
    CMAP_EncodingRecord tables[1];
} CMAP_Header;

typedef struct {
    DWORD startCharCode;
    DWORD endCharCode;
    DWORD startGlyphID;
} CMAP_SegmentedCoverage_group;

typedef struct {
    WORD format;
    WORD reserved;
    DWORD length;
    DWORD language;
    DWORD nGroups;
    CMAP_SegmentedCoverage_group groups[1];
} CMAP_SegmentedCoverage;

/* These are all structures needed for the GDEF table */
#define GDEF_TAG MS_MAKE_TAG('G', 'D', 'E', 'F')

enum {BaseGlyph=1, LigatureGlyph, MarkGlyph, ComponentGlyph};

typedef struct {
    DWORD Version;
    WORD GlyphClassDef;
    WORD AttachList;
    WORD LigCaretList;
    WORD MarkAttachClassDef;
} GDEF_Header;

typedef struct {
    WORD ClassFormat;
    WORD StartGlyph;
    WORD GlyphCount;
    WORD ClassValueArray[1];
} GDEF_ClassDefFormat1;

typedef struct {
    WORD Start;
    WORD End;
    WORD Class;
} GDEF_ClassRangeRecord;

typedef struct {
    WORD ClassFormat;
    WORD ClassRangeCount;
    GDEF_ClassRangeRecord ClassRangeRecord[1];
} GDEF_ClassDefFormat2;

/**********
 * CMAP
 **********/

static VOID *load_CMAP_format12_table(HDC hdc, ScriptCache *psc)
{
    CMAP_Header *CMAP_Table = NULL;
    int length;
    int i;

    if (!psc->CMAP_Table)
    {
        length = GetFontData(hdc, CMAP_TAG , 0, NULL, 0);
        if (length != GDI_ERROR)
        {
            psc->CMAP_Table = HeapAlloc(GetProcessHeap(),0,length);
            GetFontData(hdc, CMAP_TAG , 0, psc->CMAP_Table, length);
            TRACE("Loaded cmap table of %i bytes\n",length);
        }
        else
            return NULL;
    }

    CMAP_Table = psc->CMAP_Table;

    for (i = 0; i < GET_BE_WORD(CMAP_Table->numTables); i++)
    {
        if ( (GET_BE_WORD(CMAP_Table->tables[i].platformID) == 3) &&
             (GET_BE_WORD(CMAP_Table->tables[i].encodingID) == 10) )
        {
            CMAP_SegmentedCoverage *format = (CMAP_SegmentedCoverage*)(((BYTE*)CMAP_Table) + GET_BE_DWORD(CMAP_Table->tables[i].offset));
            if (GET_BE_WORD(format->format) == 12)
                return format;
        }
    }
    return NULL;
}

static int compare_group(const void *a, const void* b)
{
    const DWORD *chr = a;
    const CMAP_SegmentedCoverage_group *group = b;

    if (*chr < GET_BE_DWORD(group->startCharCode))
        return -1;
    if (*chr > GET_BE_DWORD(group->endCharCode))
        return 1;
    return 0;
}

DWORD OpenType_CMAP_GetGlyphIndex(HDC hdc, ScriptCache *psc, DWORD utf32c, LPWORD pgi, DWORD flags)
{
    /* BMP: use gdi32 for ease */
    if (utf32c < 0x10000)
    {
        WCHAR ch = utf32c;
        return GetGlyphIndicesW(hdc,&ch, 1, pgi, flags);
    }

    if (!psc->CMAP_format12_Table)
        psc->CMAP_format12_Table = load_CMAP_format12_table(hdc, psc);

    if (flags & GGI_MARK_NONEXISTING_GLYPHS)
        *pgi = 0xffff;
    else
        *pgi = 0;

    if (psc->CMAP_format12_Table)
    {
        CMAP_SegmentedCoverage *format = NULL;
        CMAP_SegmentedCoverage_group *group = NULL;

        format = (CMAP_SegmentedCoverage *)psc->CMAP_format12_Table;

        group = bsearch(&utf32c, format->groups, GET_BE_DWORD(format->nGroups),
                        sizeof(CMAP_SegmentedCoverage_group), compare_group);

        if (group)
        {
            DWORD offset = utf32c - GET_BE_DWORD(group->startCharCode);
            *pgi = GET_BE_DWORD(group->startGlyphID) + offset;
            return 0;
        }
    }
    return 0;
}

/**********
 * GDEF
 **********/

static WORD GDEF_get_glyph_class(const GDEF_Header *header, WORD glyph)
{
    int offset;
    WORD class = 0;
    const GDEF_ClassDefFormat1 *cf1;

    if (!header)
        return 0;

    offset = GET_BE_WORD(header->GlyphClassDef);
    if (!offset)
        return 0;

    cf1 = (GDEF_ClassDefFormat1*)(((BYTE*)header)+offset);
    if (GET_BE_WORD(cf1->ClassFormat) == 1)
    {
        if (glyph >= GET_BE_WORD(cf1->StartGlyph))
        {
            int index = glyph - GET_BE_WORD(cf1->StartGlyph);
            if (index < GET_BE_WORD(cf1->GlyphCount))
                class = GET_BE_WORD(cf1->ClassValueArray[index]);
        }
    }
    else if (GET_BE_WORD(cf1->ClassFormat) == 2)
    {
        const GDEF_ClassDefFormat2 *cf2 = (GDEF_ClassDefFormat2*)cf1;
        int i, top;
        top = GET_BE_WORD(cf2->ClassRangeCount);
        for (i = 0; i < top; i++)
        {
            if (glyph >= GET_BE_WORD(cf2->ClassRangeRecord[i].Start) &&
                glyph <= GET_BE_WORD(cf2->ClassRangeRecord[i].End))
            {
                class = GET_BE_WORD(cf2->ClassRangeRecord[i].Class);
                break;
            }
        }
    }
    else
        ERR("Unknown Class Format %i\n",GET_BE_WORD(cf1->ClassFormat));

    return class;
}

static VOID *load_gdef_table(HDC hdc)
{
    VOID* GDEF_Table = NULL;
    int length = GetFontData(hdc, GDEF_TAG , 0, NULL, 0);
    if (length != GDI_ERROR)
    {
        GDEF_Table = HeapAlloc(GetProcessHeap(),0,length);
        GetFontData(hdc, GDEF_TAG , 0, GDEF_Table, length);
        TRACE("Loaded GDEF table of %i bytes\n",length);
    }
    return GDEF_Table;
}

void OpenType_GDEF_UpdateGlyphProps(HDC hdc, ScriptCache *psc, const WORD *pwGlyphs, const WORD cGlyphs, WORD* pwLogClust, const WORD cChars, SCRIPT_GLYPHPROP *pGlyphProp)
{
    int i;

    if (!psc->GDEF_Table)
        psc->GDEF_Table = load_gdef_table(hdc);

    for (i = 0; i < cGlyphs; i++)
    {
        WORD class;
        int char_count = 0;
        int k;

        for (k = 0; k < cChars; k++)
            if (pwLogClust[k] == i)
                char_count++;

        class = GDEF_get_glyph_class(psc->GDEF_Table, pwGlyphs[i]);

        switch (class)
        {
            case 0:
            case BaseGlyph:
                pGlyphProp[i].sva.fClusterStart = 1;
                pGlyphProp[i].sva.fDiacritic = 0;
                pGlyphProp[i].sva.fZeroWidth = 0;
                break;
            case LigatureGlyph:
                pGlyphProp[i].sva.fClusterStart = 1;
                pGlyphProp[i].sva.fDiacritic = 0;
                pGlyphProp[i].sva.fZeroWidth = 0;
                break;
            case MarkGlyph:
                pGlyphProp[i].sva.fClusterStart = 0;
                pGlyphProp[i].sva.fDiacritic = 1;
                pGlyphProp[i].sva.fZeroWidth = 1;
                break;
            case ComponentGlyph:
                pGlyphProp[i].sva.fClusterStart = 0;
                pGlyphProp[i].sva.fDiacritic = 0;
                pGlyphProp[i].sva.fZeroWidth = 0;
                break;
            default:
                ERR("Unknown glyph class %i\n",class);
                pGlyphProp[i].sva.fClusterStart = 1;
                pGlyphProp[i].sva.fDiacritic = 0;
                pGlyphProp[i].sva.fZeroWidth = 0;
        }

        if (char_count == 0)
            pGlyphProp[i].sva.fClusterStart = 0;
    }
}
