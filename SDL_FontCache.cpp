/*
SDL_FontCache: A font cache for SDL and SDL_ttf
by Jonathan Dearborn

See SDL_FontCache.h for license info.
*/

#include "SDL_FontCache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define FC_GET_ALPHA(sdl_color) ((sdl_color).a)

#define FC_MIN(a,b) ((a) < (b)? (a) : (b))
#define FC_MAX(a,b) ((a) > (b)? (a) : (b))


#define FC_EXTRACT_VARARGS(buffer, start_args) \
{ \
    va_list lst; \
    va_start(lst, start_args); \
    vsnprintf(buffer, fc_buffer_size, start_args, lst); \
    va_end(lst); \
}

// Extra pixels of padding around each glyph to avoid linear filtering artifacts
#define FC_CACHE_PADDING 1



static Uint8 has_clip(SDL_Renderer* dest)
{
    return SDL_RenderIsClipEnabled(dest);
}

static SDL_Rect get_clip(SDL_Renderer* dest)
{
    SDL_Rect r;
    SDL_RenderGetClipRect(dest, &r);
    return r;
}

static void set_clip(SDL_Renderer* dest, SDL_Rect* rect)
{
    SDL_RenderSetClipRect(dest, rect);
}

static void set_color(SDL_Texture* src, Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    SDL_SetTextureColorMod(src, r, g, b);
    SDL_SetTextureAlphaMod(src, a);
}

static char* new_concat(const char* a, const char* b)
{
    // Create new buffer
    unsigned int size = strlen(a) + strlen(b);
    char* new_string = (char*)malloc(size+1);

    // Concatenate strings in the new buffer
    strcpy(new_string, a);
    strcat(new_string, b);

    return new_string;
}

static char* replace_concat(char** a, const char* b)
{
    char* new_string = new_concat(*a, b);
    free(*a);
    *a = new_string;
    return *a;
}


// Width of a tab in units of the space width (sorry, no tab alignment!)
static unsigned int fc_tab_width = 4;

// Shared buffer for variadic text
static char* fc_buffer = nullptr;
static unsigned int fc_buffer_size = 1024;

static Uint8 fc_has_render_target_support = 0;

// The number of fonts that has been created but not freed
static int NUM_EXISTING_FONTS = 0;

// Globals for GetString functions
static char* ASCII_STRING = nullptr;
static char* LATIN_1_STRING = nullptr;
static char* ASCII_LATIN_1_STRING = nullptr;

char* FC_GetStringASCII(void)
{
    if(ASCII_STRING == nullptr)
    {
        int i;
        char c;
        ASCII_STRING = (char*)malloc(512);
        memset(ASCII_STRING, 0, 512);
        i = 0;
        c = 32;
        while(1)
        {
            ASCII_STRING[i] = c;
            if(c == 126)
                break;
            ++i;
            ++c;
        }
    }
    return U8_strdup(ASCII_STRING);
}

char* FC_GetStringLatin1(void)
{
    if(LATIN_1_STRING == nullptr)
    {
        int i;
        unsigned char c;
        LATIN_1_STRING = (char*)malloc(512);
        memset(LATIN_1_STRING, 0, 512);
        i = 0;
        c = 0xA0;
        while(1)
        {
            LATIN_1_STRING[i] = 0xC2;
            LATIN_1_STRING[i+1] = c;
            if(c == 0xBF)
                break;
            i += 2;
            ++c;
        }
        i += 2;
        c = 0x80;
        while(1)
        {
            LATIN_1_STRING[i] = 0xC3;
            LATIN_1_STRING[i+1] = c;
            if(c == 0xBF)
                break;
            i += 2;
            ++c;
        }
    }
    return U8_strdup(LATIN_1_STRING);
}

char* FC_GetStringASCII_Latin1(void)
{
    if(ASCII_LATIN_1_STRING == nullptr)
		ASCII_LATIN_1_STRING = new_concat(FC_GetStringASCII(), FC_GetStringLatin1());

    return U8_strdup(ASCII_LATIN_1_STRING);
}

FC_Effect FC_MakeEffect(FC_AlignEnum alignment, FC_Scale scale, SDL_Color color)
{
    FC_Effect e;

    e.alignment = alignment;
    e.scale = scale;
    e.color = color;

    return e;
}

FC_GlyphData FC_MakeGlyphData(int cache_level, Sint16 x, Sint16 y, Uint16 w, Uint16 h)
{
    FC_GlyphData gd;

    gd.rect.x = x;
    gd.rect.y = y;
    gd.rect.w = w;
    gd.rect.h = h;
    gd.cache_level = cache_level;

    return gd;
}

// Enough to hold all of the ascii characters and some.
#define FC_DEFAULT_NUM_BUCKETS 300

typedef struct FC_MapNode
{
    Uint32 key;
    FC_GlyphData value;
    struct FC_MapNode* next;

} FC_MapNode;

typedef struct FC_Map
{
    int num_buckets;
    FC_MapNode** buckets;
} FC_Map;



static FC_Map* FC_MapCreate(int num_buckets)
{
    int i;
    FC_Map* map = (FC_Map*)malloc(sizeof(FC_Map));

    map->num_buckets = num_buckets;
    map->buckets = (FC_MapNode**)malloc(num_buckets * sizeof(FC_MapNode*));

    for(i = 0; i < num_buckets; ++i)
    {
        map->buckets[i] = nullptr;
    }

    return map;
}

static void FC_MapFree(FC_Map* map)
{
    int i;
    if(map == nullptr)
        return;

    // Go through each bucket
    for(i = 0; i < map->num_buckets; ++i)
    {
        // Delete the nodes in order
        FC_MapNode* node = map->buckets[i];
        while(node != nullptr)
        {
            FC_MapNode* last = node;
            node = node->next;
            free(last);
        }
    }

    free(map->buckets);
    free(map);
}

// Note: Does not handle duplicates in any special way.
static FC_GlyphData* FC_MapInsert(FC_Map* map, Uint32 codepoint, FC_GlyphData glyph)
{
    Uint32 index;
    FC_MapNode* node;
    if(map == nullptr)
        return nullptr;

    // Get index for bucket
    index = codepoint % map->num_buckets;

    // If this bucket is empty, create a node and return its value
    if(map->buckets[index] == nullptr)
    {
        node = map->buckets[index] = (FC_MapNode*)malloc(sizeof(FC_MapNode));
        node->key = codepoint;
        node->value = glyph;
        node->next = nullptr;
        return &node->value;
    }

    for(node = map->buckets[index]; node != nullptr; node = node->next)
    {
        // Find empty node and add a new one on.
        if(node->next == nullptr)
        {
            node->next = (FC_MapNode*)malloc(sizeof(FC_MapNode));
            node = node->next;

            node->key = codepoint;
            node->value = glyph;
            node->next = nullptr;
            return &node->value;
        }
    }

    return nullptr;
}

static FC_GlyphData* FC_MapFind(FC_Map* map, Uint32 codepoint)
{
    Uint32 index;
    FC_MapNode* node;
    if(map == nullptr)
        return nullptr;

    // Get index for bucket
    index = codepoint % map->num_buckets;

    // Go through list until we find a match
    for(node = map->buckets[index]; node != nullptr; node = node->next)
    {
        if(node->key == codepoint)
            return &node->value;
    }

    return nullptr;
}



struct FC_Font
{
    SDL_Renderer* renderer;

    TTF_Font* ttf_source;  // TTF_Font source of characters
    Uint8 owns_ttf_source;  // Can we delete the TTF_Font ourselves?

    FC_FilterEnum filter;

    SDL_Color default_color;
    Uint16 height;

    Uint16 maxWidth;
    Uint16 baseline;
    int ascent;
    int descent;

    int lineSpacing;
    int letterSpacing;

    // Uses 32-bit (4-byte) Unicode codepoints to refer to each glyph
    // Codepoints are little endian (reversed from UTF-8) so that something like 0x00000005 is ASCII 5 and the map can be indexed by ASCII values
    FC_Map* glyphs;

    FC_GlyphData last_glyph;  // Texture packing cursor
    int glyph_cache_size;
    int glyph_cache_count;
    SDL_Texture** glyph_cache;

    char* loading_string;

};

// Private
static FC_GlyphData* FC_PackGlyphData(FC_Font* font, Uint32 codepoint, Uint16 width, Uint16 maxWidth, Uint16 maxHeight);


static SDL_Rect FC_RenderLeft(FC_Font* font, SDL_Renderer* dest, int x, int y, FC_Scale scale, const char* text);
static SDL_Rect FC_RenderCenter(FC_Font* font, SDL_Renderer* dest, int x, int y, FC_Scale scale, const char* text);
static SDL_Rect FC_RenderRight(FC_Font* font, SDL_Renderer* dest, int x, int y, FC_Scale scale, const char* text);


static inline SDL_Surface* FC_CreateSurface32(Uint32 width, Uint32 height)
{
    #if SDL_BYTEORDER == SDL_BIG_ENDIAN
        return SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 32, 0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF);
    #else
        return SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    #endif
}


char* U8_alloc(unsigned int size)
{
    char* result;
    if(size == 0)
        return nullptr;

    result = (char*)malloc(size);
    result[0] = '\0';

    return result;
}

void U8_free(char* string)
{
    free(string);
}

char* U8_strdup(const char* string)
{
    char* result;
    if(string == nullptr)
        return nullptr;

    result = (char*)malloc(strlen(string)+1);
    strcpy(result, string);

    return result;
}

int U8_strlen(const char* string)
{
    int length = 0;
    if(string == nullptr)
        return 0;

    while(*string != '\0')
    {
        string = U8_next(string);
        ++length;
    }

    return length;
}

int U8_charsize(const char* character)
{
    if(character == nullptr)
        return 0;

    if((unsigned char)*character <= 0x7F)
        return 1;
    else if((unsigned char)*character < 0xE0)
        return 2;
    else if((unsigned char)*character < 0xF0)
        return 3;
    else
        return 4;
    return 1;
}

int U8_charcpy(char* buffer, const char* source, int buffer_size)
{
    int charsize;
    if(buffer == nullptr || source == nullptr || buffer_size < 1)
        return 0;

    charsize = U8_charsize(source);
    if(charsize > buffer_size)
        return 0;

    memcpy(buffer, source, charsize);
    return charsize;
}

const char* U8_next(const char* string)
{
    return string + U8_charsize(string);
}

int U8_strinsert(char* string, int position, const char* source, int max_bytes)
{
    int pos_u8char;
    int len;
    int add_len;
    int ulen;
    const char* string_start = string;

    if(string == nullptr || source == nullptr)
        return 0;

    len = strlen(string);
    add_len = strlen(source);
    ulen = U8_strlen(string);

    if(position == -1)
        position = ulen;

    if(position < 0 || position > ulen || len + add_len + 1 > max_bytes)
        return 0;

    // Move string pointer to the proper position
    pos_u8char = 0;
    while(*string != '\0' && pos_u8char < position)
    {
        string = (char*)U8_next(string);
        ++pos_u8char;
    }

    // Move the rest of the string out of the way
    memmove(string + add_len, string, len - (string - string_start) + 1);

    // Copy in the new characters
    memcpy(string, source, add_len);

    return 1;
}

void U8_strdel(char* string, int position)
{
    if(string == nullptr || position < 0)
        return;

    while(*string != '\0')
    {
        if(position == 0)
        {
            int chars_to_erase = U8_charsize(string);
            int remaining_bytes = strlen(string) + 1;
            memmove(string, string + chars_to_erase, remaining_bytes);
            break;
        }

        string = (char*)U8_next(string);
        --position;
    }
}

static inline SDL_Rect SDL_RectUnion(SDL_Rect A, SDL_Rect B)
{
    int x,x2,y,y2;
    x = FC_MIN(A.x, B.x);
    y = FC_MIN(A.y, B.y);
    x2 = FC_MAX(A.x+A.w, B.x+B.w);
    y2 = FC_MAX(A.y+A.h, B.y+B.h);
    {
        SDL_Rect result = {x, y, FC_MAX(0, x2 - x), FC_MAX(0, y2 - y)};
        return result;
    }
}

// Adapted from SDL_IntersectRect
static inline SDL_Rect SDL_RectIntersect(SDL_Rect A, SDL_Rect B)
{
    SDL_Rect result;
	float Amin, Amax, Bmin, Bmax;

	// Horizontal intersection
	Amin = A.x;
	Amax = Amin + A.w;
	Bmin = B.x;
	Bmax = Bmin + B.w;
	if(Bmin > Amin)
	        Amin = Bmin;
	result.x = Amin;
	if(Bmax < Amax)
	        Amax = Bmax;
	result.w = Amax - Amin > 0 ? Amax - Amin : 0;

	// Vertical intersection
	Amin = A.y;
	Amax = Amin + A.h;
	Bmin = B.y;
	Bmax = Bmin + B.h;
	if(Bmin > Amin)
	        Amin = Bmin;
	result.y = Amin;
	if(Bmax < Amax)
	        Amax = Bmax;
	result.h = Amax - Amin > 0 ? Amax - Amin : 0;

	return result;
}














SDL_Rect FC_DefaultRenderCallback(SDL_Texture* src, SDL_Rect* srcrect, SDL_Renderer* dest, int x, int y, float xscale, float yscale)
{
    float w = srcrect->w * xscale;
    float h = srcrect->h * yscale;
    SDL_Rect result;

    // FIXME: Why does the scaled offset look so wrong?
    {
        SDL_RendererFlip flip = SDL_FLIP_NONE;
        if(xscale < 0)
        {
            xscale = -xscale;
            flip = (SDL_RendererFlip) ((int)flip | (int)SDL_FLIP_HORIZONTAL);
        }
        if(yscale < 0)
        {
            yscale = -yscale;
            flip = (SDL_RendererFlip) ((int)flip | (int)SDL_FLIP_VERTICAL);
        }

        SDL_Rect r = *srcrect;
        SDL_Rect dr = {(int)x, (int)y, (int)(xscale*r.w), (int)(yscale*r.h)};
        SDL_RenderCopyEx(dest, src, &r, &dr, 0, nullptr, flip);
    }

    result.x = x;
    result.y = y;
    result.w = w;
    result.h = h;
    return result;
}

static SDL_Rect (*fc_render_callback)(SDL_Texture* src, SDL_Rect* srcrect, SDL_Renderer* dest, int x, int y, float xscale, float yscale) = &FC_DefaultRenderCallback;

void FC_SetRenderCallback(SDL_Rect (*callback)(SDL_Texture* src, SDL_Rect* srcrect, SDL_Renderer* dest, int x, int y, float xscale, float yscale))
{
    if(callback == nullptr)
        fc_render_callback = &FC_DefaultRenderCallback;
    else
        fc_render_callback = callback;
}

void FC_GetUTF8FromCodepoint(char* result, Uint32 codepoint)
{
    char a, b, c, d;

    if(result == nullptr)
        return;

    a = (codepoint >> 24) & 0xFF;
    b = (codepoint >> 16) & 0xFF;
    c = (codepoint >> 8) & 0xFF;
    d = codepoint & 0xFF;

    if(a == 0)
    {
        if(b == 0)
        {
            if(c == 0)
            {
                result[0] = d;
                result[1] = '\0';
            }
            else
            {
                result[0] = c;
                result[1] = d;
                result[2] = '\0';
            }
        }
        else
        {
            result[0] = b;
            result[1] = c;
            result[2] = d;
            result[3] = '\0';
        }
    }
    else
    {
        result[0] = a;
        result[1] = b;
        result[2] = c;
        result[3] = d;
        result[4] = '\0';
    }
}

Uint32 FC_GetCodepointFromUTF8(const char** c, Uint8 advance_pointer)
{
    Uint32 result = 0;
    const char* str;
    if(c == nullptr || *c == nullptr)
        return 0;

    str = *c;
    if((unsigned char)*str <= 0x7F)
        result = *str;
    else if((unsigned char)*str < 0xE0)
    {
        result |= (unsigned char)(*str) << 8;
        result |= (unsigned char)(*(str+1));
        if(advance_pointer)
            *c += 1;
    }
    else if((unsigned char)*str < 0xF0)
    {
        result |= (unsigned char)(*str) << 16;
        result |= (unsigned char)(*(str+1)) << 8;
        result |= (unsigned char)(*(str+2));
        if(advance_pointer)
            *c += 2;
    }
    else
    {
        result |= (unsigned char)(*str) << 24;
        result |= (unsigned char)(*(str+1)) << 16;
        result |= (unsigned char)(*(str+2)) << 8;
        result |= (unsigned char)(*(str+3));
        if(advance_pointer)
            *c += 3;
    }
    return result;
}


void FC_SetLoadingString(FC_Font* font, const char* string)
{
    if(font == nullptr)
        return;

    free(font->loading_string);
    font->loading_string = U8_strdup(string);
}


unsigned int FC_GetBufferSize(void)
{
    return fc_buffer_size;
}

void FC_SetBufferSize(unsigned int size)
{
    free(fc_buffer);
    if(size > 0)
    {
        fc_buffer_size = size;
        fc_buffer = (char*)malloc(fc_buffer_size);
    }
    else
        fc_buffer = (char*)malloc(fc_buffer_size);
}


unsigned int FC_GetTabWidth(void)
{
    return fc_tab_width;
}

void FC_SetTabWidth(unsigned int width_in_spaces)
{
    fc_tab_width = width_in_spaces;
}





// Constructors

static void FC_Init(FC_Font* font)
{
    if(font == nullptr)
        return;

    font->renderer = nullptr;

    font->ttf_source = nullptr;
    font->owns_ttf_source = 0;

    font->filter = FC_FILTER_NEAREST;

    font->default_color.r = 0;
    font->default_color.g = 0;
    font->default_color.b = 0;
    FC_GET_ALPHA(font->default_color) = 255;

    font->height = 0; // ascent+descent

    font->maxWidth = 0;
    font->baseline = 0;
    font->ascent = 0;
    font->descent = 0;

    font->lineSpacing = 0;
    font->letterSpacing = 0;

    // Give a little offset for when filtering/mipmaps are used.  Depending on mipmap level, this will still not be enough.
    font->last_glyph.rect.x = FC_CACHE_PADDING;
    font->last_glyph.rect.y = FC_CACHE_PADDING;
    font->last_glyph.rect.w = 0;
    font->last_glyph.rect.h = 0;
    font->last_glyph.cache_level = 0;

    if(font->glyphs != nullptr)
        FC_MapFree(font->glyphs);

    font->glyphs = FC_MapCreate(FC_DEFAULT_NUM_BUCKETS);

    font->glyph_cache_size = 3;
    font->glyph_cache_count = 0;


    font->glyph_cache = (SDL_Texture**)malloc(font->glyph_cache_size * sizeof(SDL_Texture*));

	if (font->loading_string == nullptr)
		font->loading_string = FC_GetStringASCII();

    if(fc_buffer == nullptr)
        fc_buffer = (char*)malloc(fc_buffer_size);
}

static Uint8 FC_GrowGlyphCache(FC_Font* font)
{
    if(font == nullptr)
        return 0;

    SDL_Texture* new_level = SDL_CreateTexture(font->renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, font->height * 12, font->height * 12);
    
    if(new_level == nullptr || !FC_SetGlyphCacheLevel(font, font->glyph_cache_count, new_level))
    {
        SDL_Log("Error: SDL_FontCache ran out of packing space and could not add another cache level.\n");
        SDL_DestroyTexture(new_level);
        
        return 0;
    }
    // bug: we do not have the correct color here, this might be the wrong color!
    //      , most functions use set_color_for_all_caches()
    //   - for evading this bug, you must use FC_SetDefaultColor(), before using any draw functions
    set_color(new_level, font->default_color.r, font->default_color.g, font->default_color.b, FC_GET_ALPHA(font->default_color));
    {
        Uint8 r, g, b, a;
        SDL_Texture* prev_target = SDL_GetRenderTarget(font->renderer);
        SDL_Rect prev_clip, prev_viewport;
        int prev_logicalw, prev_logicalh;
        Uint8 prev_clip_enabled;
        float prev_scalex, prev_scaley;
        // only backup if previous target existed (SDL will preserve them for the default target)
        if (prev_target) {
            prev_clip_enabled = has_clip(font->renderer);
            if (prev_clip_enabled)
                prev_clip = get_clip(font->renderer);
            SDL_RenderGetViewport(font->renderer, &prev_viewport);
            SDL_RenderGetScale(font->renderer, &prev_scalex, &prev_scaley);
            SDL_RenderGetLogicalSize(font->renderer, &prev_logicalw, &prev_logicalh);
        }
        SDL_SetTextureBlendMode(new_level, SDL_BLENDMODE_BLEND);
        SDL_SetRenderTarget(font->renderer, new_level);
        SDL_GetRenderDrawColor(font->renderer, &r, &g, &b, &a);
        SDL_SetRenderDrawColor(font->renderer, 0, 0, 0, 0);
        SDL_RenderClear(font->renderer);
        SDL_SetRenderDrawColor(font->renderer, r, g, b, a);
        SDL_SetRenderTarget(font->renderer, prev_target);
        if (prev_target) {
            if (prev_clip_enabled)
                set_clip(font->renderer, &prev_clip);
            if (prev_logicalw && prev_logicalh)
                SDL_RenderSetLogicalSize(font->renderer, prev_logicalw, prev_logicalh);
            else {
                SDL_RenderSetViewport(font->renderer, &prev_viewport);
                SDL_RenderSetScale(font->renderer, prev_scalex, prev_scaley);
            }
        }
    }

    return 1;
}

Uint8 FC_UploadGlyphCache(FC_Font* font, int cache_level, SDL_Surface* data_surface)
{
    if(font == nullptr || data_surface == nullptr)
        return 0;

    SDL_Texture* new_level;
    if(!fc_has_render_target_support)
        new_level = SDL_CreateTextureFromSurface(font->renderer, data_surface);
    else
    {
        // Must upload with render target enabled so we can put more glyphs on later
        SDL_Renderer* renderer = font->renderer;

        // Set filter mode for new texture
        char old_filter_mode[16];  // Save it so we can change the hint value in the meantime
        const char* old_filter_hint = SDL_GetHint(SDL_HINT_RENDER_SCALE_QUALITY);
        if(!old_filter_hint)
            old_filter_hint = "nearest";
        snprintf(old_filter_mode, 16, "%s", old_filter_hint);

        if(FC_GetFilterMode(font) == FC_FILTER_LINEAR)
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        else
            SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

        new_level = SDL_CreateTexture(renderer, data_surface->format->format, SDL_TEXTUREACCESS_TARGET, data_surface->w, data_surface->h);
        SDL_SetTextureBlendMode(new_level, SDL_BLENDMODE_BLEND);

        // Reset filter mode for the temp texture
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

        {
            Uint8 r, g, b, a;
            SDL_Texture* temp = SDL_CreateTextureFromSurface(renderer, data_surface);
            SDL_Texture* prev_target = SDL_GetRenderTarget(renderer);
            SDL_Rect prev_clip, prev_viewport;
            int prev_logicalw, prev_logicalh;
            Uint8 prev_clip_enabled;
            float prev_scalex, prev_scaley;
            // only backup if previous target existed (SDL will preserve them for the default target)
            if (prev_target) {
                prev_clip_enabled = has_clip(renderer);
                if (prev_clip_enabled)
                    prev_clip = get_clip(renderer);
                SDL_RenderGetViewport(renderer, &prev_viewport);
                SDL_RenderGetScale(renderer, &prev_scalex, &prev_scaley);
                SDL_RenderGetLogicalSize(renderer, &prev_logicalw, &prev_logicalh);
            }
            SDL_SetTextureBlendMode(temp, SDL_BLENDMODE_NONE);
            SDL_SetRenderTarget(renderer, new_level);

            SDL_GetRenderDrawColor(renderer, &r, &g, &b, &a);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
            SDL_RenderClear(renderer);
            SDL_SetRenderDrawColor(renderer, r, g, b, a);

            SDL_RenderCopy(renderer, temp, nullptr, nullptr);
            SDL_SetRenderTarget(renderer, prev_target);
            if (prev_target) {
                if (prev_clip_enabled)
                    set_clip(renderer, &prev_clip);
                if (prev_logicalw && prev_logicalh)
                    SDL_RenderSetLogicalSize(renderer, prev_logicalw, prev_logicalh);
                else {
                    SDL_RenderSetViewport(renderer, &prev_viewport);
                    SDL_RenderSetScale(renderer, prev_scalex, prev_scaley);
                }
            }

            SDL_DestroyTexture(temp);
        }

        // Reset to the old filter value
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, old_filter_mode);

    }
    
    if(new_level == nullptr || !FC_SetGlyphCacheLevel(font, cache_level, new_level))
    {
        SDL_Log("Error: SDL_FontCache ran out of packing space and could not add another cache level.\n");
        SDL_DestroyTexture(new_level);
        
        return 0;
    }
    return 1;
}

static FC_GlyphData* FC_PackGlyphData(FC_Font* font, Uint32 codepoint, Uint16 width, Uint16 maxWidth, Uint16 maxHeight)
{
    FC_Map* glyphs = font->glyphs;
    FC_GlyphData* last_glyph = &font->last_glyph;
    Uint16 height = font->height + FC_CACHE_PADDING;

    // TAB is special!
    if(codepoint == '\t')
    {
        FC_GlyphData spaceGlyph;
        FC_GetGlyphData(font, &spaceGlyph, ' ');
        width = fc_tab_width * spaceGlyph.rect.w;
    }

    if(last_glyph->rect.x + last_glyph->rect.w + width >= maxWidth - FC_CACHE_PADDING)
    {
        if(last_glyph->rect.y + height + height >= maxHeight - FC_CACHE_PADDING)
        {
            // Get ready to pack on the next cache level when it is ready
            last_glyph->cache_level = font->glyph_cache_count;
            last_glyph->rect.x = FC_CACHE_PADDING;
            last_glyph->rect.y = FC_CACHE_PADDING;
            last_glyph->rect.w = 0;
            return nullptr;
        }
        else
        {
            // Go to next row
            last_glyph->rect.x = FC_CACHE_PADDING;
            last_glyph->rect.y += height;
            last_glyph->rect.w = 0;
        }
    }

    // Move to next space
    last_glyph->rect.x += last_glyph->rect.w + 1 + FC_CACHE_PADDING;
    last_glyph->rect.w = width;

    return FC_MapInsert(glyphs, codepoint, FC_MakeGlyphData(last_glyph->cache_level, last_glyph->rect.x, last_glyph->rect.y, last_glyph->rect.w, last_glyph->rect.h));
}


SDL_Texture* FC_GetGlyphCacheLevel(FC_Font* font, int cache_level)
{
    if(font == nullptr || cache_level < 0 || cache_level > font->glyph_cache_count)
        return nullptr;

    return font->glyph_cache[cache_level];
}

Uint8 FC_SetGlyphCacheLevel(FC_Font* font, int cache_level, SDL_Texture* cache_texture)
{
    if(font == nullptr || cache_level < 0)
        return 0;

    // Must be sequentially added
    if(cache_level > font->glyph_cache_count + 1)
        return 0;

    if(cache_level == font->glyph_cache_count)
    {
        font->glyph_cache_count++;

        // Grow cache?
        if(font->glyph_cache_count > font->glyph_cache_size)
        {
            // Copy old cache to new one
            int i;
            SDL_Texture** new_cache;
            new_cache = (SDL_Texture**)malloc(font->glyph_cache_count * sizeof(SDL_Texture*));
            for(i = 0; i < font->glyph_cache_size; ++i)
                new_cache[i] = font->glyph_cache[i];

            // Save new cache
            free(font->glyph_cache);
            font->glyph_cache_size = font->glyph_cache_count;
            font->glyph_cache = new_cache;
        }
    }

    font->glyph_cache[cache_level] = cache_texture;
    return 1;
}


FC_Font* FC_CreateFont(void)
{
    FC_Font* font;

    font = (FC_Font*)malloc(sizeof(FC_Font));
    memset(font, 0, sizeof(FC_Font));

    FC_Init(font);
    ++NUM_EXISTING_FONTS;

    return font;
}


// Assume this many will be enough...
#define FC_LOAD_MAX_SURFACES 10

Uint8 FC_LoadFontFromTTF(FC_Font* font, SDL_Renderer* renderer, TTF_Font* ttf, SDL_Color color)
{
    if(font == nullptr || ttf == nullptr)
        return 0;
    
    if(renderer == nullptr)
        return 0;
    
    FC_ClearFont(font);


    // Might as well check render target support here
    SDL_RendererInfo info;
    SDL_GetRendererInfo(renderer, &info);
    fc_has_render_target_support = (info.flags & SDL_RENDERER_TARGETTEXTURE);

    font->renderer = renderer;

    font->ttf_source = ttf;

    //font->line_height = TTF_FontLineSkip(ttf);
    font->height = TTF_FontHeight(ttf);
    font->ascent = TTF_FontAscent(ttf);
    font->descent = -TTF_FontDescent(ttf);

    // Some bug for certain fonts can result in an incorrect height.
    if(font->height < font->ascent - font->descent)
        font->height = font->ascent - font->descent;

    font->baseline = font->height - font->descent;

    font->default_color = color;

    {
        SDL_Color white = {255, 255, 255, 255};
        SDL_Surface* glyph_surf;
        char buff[5];
        const char* buff_ptr = buff;
        const char* source_string;
        Uint8 packed = 0;

        // Copy glyphs from the surface to the font texture and store the position data
        // Pack row by row into a square texture
        // Try figuring out dimensions that make sense for the font size.
        unsigned int w = font->height*12;
        unsigned int h = font->height*12;
        SDL_Surface* surfaces[FC_LOAD_MAX_SURFACES];
        int num_surfaces = 1;
        surfaces[0] = FC_CreateSurface32(w, h);
        font->last_glyph.rect.x = FC_CACHE_PADDING;
        font->last_glyph.rect.y = FC_CACHE_PADDING;
        font->last_glyph.rect.w = 0;
        font->last_glyph.rect.h = font->height;

        source_string = font->loading_string;
        for(; *source_string != '\0'; source_string = U8_next(source_string))
        {
            memset(buff, 0, 5);
            if(!U8_charcpy(buff, source_string, 5))
                continue;
            glyph_surf = TTF_RenderUTF8_Blended(ttf, buff, white);
            if(glyph_surf == nullptr)
                continue;

            // Try packing.  If it fails, create a new surface for the next cache level.
            packed = (FC_PackGlyphData(font, FC_GetCodepointFromUTF8(&buff_ptr, 0), glyph_surf->w, surfaces[num_surfaces-1]->w, surfaces[num_surfaces-1]->h) != nullptr);
            if(!packed)
            {
                int i = num_surfaces-1;
                if(num_surfaces >= FC_LOAD_MAX_SURFACES)
                {
                    // Can't do any more!
                    SDL_Log("SDL_FontCache error: Could not create enough cache surfaces to fit all of the loading string!\n");
                    SDL_FreeSurface(glyph_surf);
                    break;
                }

                // Upload the current surface to the glyph cache now so we can keep the cache level packing cursor up to date as we go.
                FC_UploadGlyphCache(font, i, surfaces[i]);
                SDL_FreeSurface(surfaces[i]);
                
                SDL_SetTextureBlendMode(font->glyph_cache[i], SDL_BLENDMODE_BLEND);
                
                // Update the glyph cursor to the new cache level.  We need to do this here because the actual cache lags behind our use of the packing above.
                font->last_glyph.cache_level = num_surfaces;


                surfaces[num_surfaces] = FC_CreateSurface32(w, h);
                num_surfaces++;
            }

            // Try packing for the new surface, then blit onto it.
            if(packed || FC_PackGlyphData(font, FC_GetCodepointFromUTF8(&buff_ptr, 0), glyph_surf->w, surfaces[num_surfaces-1]->w, surfaces[num_surfaces-1]->h) != nullptr)
            {
                SDL_SetSurfaceBlendMode(glyph_surf, SDL_BLENDMODE_NONE);
                SDL_Rect srcRect = {0, 0, glyph_surf->w, glyph_surf->h};
                SDL_Rect destrect = font->last_glyph.rect;
                SDL_BlitSurface(glyph_surf, &srcRect, surfaces[num_surfaces-1], &destrect);
            }

            SDL_FreeSurface(glyph_surf);
        }

        {
            int i = num_surfaces-1;
            FC_UploadGlyphCache(font, i, surfaces[i]);
            SDL_FreeSurface(surfaces[i]);
            SDL_SetTextureBlendMode(font->glyph_cache[i], SDL_BLENDMODE_BLEND);
        }
    }

    return 1;
}

Uint8 FC_LoadFont(FC_Font* font, SDL_Renderer* renderer, const char* filename_ttf, Uint32 pointSize, SDL_Color color, int style)
{
    SDL_RWops* rwops;

    if(font == nullptr)
        return 0;

    rwops = SDL_RWFromFile(filename_ttf, "rb");

    if(rwops == nullptr)
    {
        SDL_Log("Unable to open file for reading: %s \n", SDL_GetError());
        return 0;
    }

    return FC_LoadFont_RW(font, renderer, rwops, 1, pointSize, color, style);
}

Uint8 FC_LoadFont_RW(FC_Font* font, SDL_Renderer* renderer, SDL_RWops* file_rwops_ttf, Uint8 own_rwops, Uint32 pointSize, SDL_Color color, int style)
{
    Uint8 result;
    TTF_Font* ttf;
    Uint8 outline;

    if(font == nullptr)
        return 0;

    if(!TTF_WasInit() && TTF_Init() < 0)
    {
        SDL_Log("Unable to initialize SDL_ttf: %s \n", TTF_GetError());
        if(own_rwops)
            SDL_RWclose(file_rwops_ttf);
        return 0;
    }

    ttf = TTF_OpenFontRW(file_rwops_ttf, own_rwops, pointSize);

    if(ttf == nullptr)
    {
        SDL_Log("Unable to load TrueType font: %s \n", TTF_GetError());
        if(own_rwops)
            SDL_RWclose(file_rwops_ttf);
        return 0;
    }

    outline = (style & TTF_STYLE_OUTLINE);
    if(outline)
    {
        style &= ~TTF_STYLE_OUTLINE;
        TTF_SetFontOutline(ttf, 1);
    }
    TTF_SetFontStyle(ttf, style);

    result = FC_LoadFontFromTTF(font, renderer, ttf, color);

    // Can only load new (uncached) glyphs if we can keep the SDL_RWops open.
    font->owns_ttf_source = own_rwops;
    if(!own_rwops)
    {
        TTF_CloseFont(font->ttf_source);
        font->ttf_source = nullptr;
    }

    return result;
}

void FC_ClearFont(FC_Font* font)
{
    int i;
    if(font == nullptr)
        return;

    // Release resources
    if(font->owns_ttf_source)
        TTF_CloseFont(font->ttf_source);

    font->owns_ttf_source = 0;
    font->ttf_source = nullptr;

    // Delete glyph map
    FC_MapFree(font->glyphs);
    font->glyphs = nullptr;

    // Delete glyph cache
    for(i = 0; i < font->glyph_cache_count; ++i)
    {
        SDL_DestroyTexture(font->glyph_cache[i]);
    }
    free(font->glyph_cache);
    font->glyph_cache = nullptr;

    // Reset font
    FC_Init(font);
}


void FC_FreeFont(FC_Font* font)
{
    int i;
    if(font == nullptr)
        return;

    // Release resources
    if(font->owns_ttf_source)
        TTF_CloseFont(font->ttf_source);

    // Delete glyph map
    FC_MapFree(font->glyphs);

    // Delete glyph cache
    for(i = 0; i < font->glyph_cache_count; ++i)
    {
        SDL_DestroyTexture(font->glyph_cache[i]);
    }
    free(font->glyph_cache);

    free(font->loading_string);

    free(font);

    // If the last font has been freed; assume shutdown and free the global variables
    if (--NUM_EXISTING_FONTS <= 0)
    {
        free(ASCII_STRING);
        ASCII_STRING = nullptr;

        free(LATIN_1_STRING);
        LATIN_1_STRING = nullptr;

        free(ASCII_LATIN_1_STRING);
        ASCII_LATIN_1_STRING = nullptr;

        free(fc_buffer);
        fc_buffer = nullptr;
    }
}

int FC_GetNumCacheLevels(FC_Font* font)
{
    return font->glyph_cache_count;
}

Uint8 FC_AddGlyphToCache(FC_Font* font, SDL_Surface* glyph_surface)
{
    if(font == nullptr || glyph_surface == nullptr)
        return 0;

    SDL_SetSurfaceBlendMode(glyph_surface, SDL_BLENDMODE_NONE);
    SDL_Texture* dest = FC_GetGlyphCacheLevel(font, font->last_glyph.cache_level);
    if(dest == nullptr)
        return 0;

    {
        SDL_Renderer* renderer = font->renderer;
        SDL_Texture* img;
        SDL_Rect destrect;
        SDL_Texture* prev_target = SDL_GetRenderTarget(renderer);
        SDL_Rect prev_clip, prev_viewport;
        int prev_logicalw, prev_logicalh;
        Uint8 prev_clip_enabled;
        float prev_scalex, prev_scaley;
        // only backup if previous target existed (SDL will preserve them for the default target)
        if (prev_target) {
            prev_clip_enabled = has_clip(renderer);
            if (prev_clip_enabled)
                prev_clip = get_clip(renderer);
            SDL_RenderGetViewport(renderer, &prev_viewport);
            SDL_RenderGetScale(renderer, &prev_scalex, &prev_scaley);
            SDL_RenderGetLogicalSize(renderer, &prev_logicalw, &prev_logicalh);
        }

        img = SDL_CreateTextureFromSurface(renderer, glyph_surface);

        destrect = font->last_glyph.rect;
        SDL_SetRenderTarget(renderer, dest);
        SDL_RenderCopy(renderer, img, nullptr, &destrect);
        SDL_SetRenderTarget(renderer, prev_target);
        if (prev_target) {
            if (prev_clip_enabled)
                set_clip(renderer, &prev_clip);
            if (prev_logicalw && prev_logicalh)
                SDL_RenderSetLogicalSize(renderer, prev_logicalw, prev_logicalh);
            else {
                SDL_RenderSetViewport(renderer, &prev_viewport);
                SDL_RenderSetScale(renderer, prev_scalex, prev_scaley);
            }
        }

        SDL_DestroyTexture(img);
    }

    return 1;
}


unsigned int FC_GetNumCodepoints(FC_Font* font)
{
    FC_Map* glyphs;
    int i;
    unsigned int result = 0;
    if(font == nullptr || font->glyphs == nullptr)
        return 0;

    glyphs = font->glyphs;

    for(i = 0; i < glyphs->num_buckets; ++i)
    {
        FC_MapNode* node;
        for(node = glyphs->buckets[i]; node != nullptr; node = node->next)
        {
            result++;
        }
    }

    return result;
}

void FC_GetCodepoints(FC_Font* font, Uint32* result)
{
    FC_Map* glyphs;
    int i;
    unsigned int count = 0;
    if(font == nullptr || font->glyphs == nullptr)
        return;

    glyphs = font->glyphs;

    for(i = 0; i < glyphs->num_buckets; ++i)
    {
        FC_MapNode* node;
        for(node = glyphs->buckets[i]; node != nullptr; node = node->next)
        {
            result[count] = node->key;
            count++;
        }
    }
}

Uint8 FC_GetGlyphData(FC_Font* font, FC_GlyphData* result, Uint32 codepoint)
{
    FC_GlyphData* e = FC_MapFind(font->glyphs, codepoint);
    if(e == nullptr)
    {
        char buff[5];
        int w, h;
        SDL_Color white = {255, 255, 255, 255};
        SDL_Surface* surf;
        SDL_Texture* cache_image;

        if(font->ttf_source == nullptr)
            return 0;

        FC_GetUTF8FromCodepoint(buff, codepoint);

        cache_image = FC_GetGlyphCacheLevel(font, font->last_glyph.cache_level);
        if(cache_image == nullptr)
        {
            SDL_Log("SDL_FontCache: Failed to load cache image, so cannot add new glyphs!\n");
            return 0;
        }

        SDL_QueryTexture(cache_image, nullptr, nullptr, &w, &h);

        surf = TTF_RenderUTF8_Blended(font->ttf_source, buff, white);
        if(surf == nullptr)
        {
            return 0;
        }

        e = FC_PackGlyphData(font, codepoint, surf->w, w, h);
        if(e == nullptr)
        {
            // Grow the cache
            FC_GrowGlyphCache(font);

            // Try packing again
            e = FC_PackGlyphData(font, codepoint, surf->w, w, h);
            if(e == nullptr)
            {
                SDL_FreeSurface(surf);
                return 0;
            }
        }

        // Render onto the cache texture
        FC_AddGlyphToCache(font, surf);

        SDL_FreeSurface(surf);
    }

    if(result != nullptr && e != nullptr)
        *result = *e;

    return 1;
}


FC_GlyphData* FC_SetGlyphData(FC_Font* font, Uint32 codepoint, FC_GlyphData glyph_data)
{
    return FC_MapInsert(font->glyphs, codepoint, glyph_data);
}



// Drawing
static SDL_Rect FC_RenderLeft(FC_Font* font, SDL_Renderer* dest, int x, int y, FC_Scale scale, const char* text)
{
    const char* c = text;
    SDL_Rect srcRect;
    SDL_Rect dstRect;
    SDL_Rect dirtyRect = {x, y, 0, 0};

    FC_GlyphData glyph;
    Uint32 codepoint;

    float destX = x;
    float destY = y;
    float destH;
    float destLineSpacing;
    float destLetterSpacing;

    if(font == nullptr)
        return dirtyRect;

    destH = font->height * scale.y;
    destLineSpacing = font->lineSpacing*scale.y;
    destLetterSpacing = font->letterSpacing*scale.x;

    if(c == nullptr || font->glyph_cache_count == 0 || dest == nullptr)
        return dirtyRect;

    int newlineX = x;

    for(; *c != '\0'; c++)
    {
        if(*c == '\n')
        {
            destX = newlineX;
            destY += destH + destLineSpacing;
            continue;
        }

        codepoint = FC_GetCodepointFromUTF8(&c, 1);  // Increments 'c' to skip the extra UTF-8 bytes
        if(!FC_GetGlyphData(font, &glyph, codepoint))
        {
            codepoint = ' ';
            if(!FC_GetGlyphData(font, &glyph, codepoint))
                continue;  // Skip bad characters
        }

        if (codepoint == ' ')
        {
            destX += glyph.rect.w*scale.x + destLetterSpacing;
            continue;
        }
        /*if(destX >= dest->w)
            continue;
        if(destY >= dest->h)
            continue;*/

        srcRect = glyph.rect;
    
        dstRect = fc_render_callback(FC_GetGlyphCacheLevel(font, glyph.cache_level), &srcRect, dest, destX, destY, scale.x, scale.y);
        if(dirtyRect.w == 0 || dirtyRect.h == 0)
            dirtyRect = dstRect;
        else
            dirtyRect = SDL_RectUnion(dirtyRect, dstRect);

        destX += glyph.rect.w*scale.x + destLetterSpacing;
    }

    return dirtyRect;
}

static void set_color_for_all_caches(FC_Font* font, SDL_Color color)
{
    // TODO: How can I predict which glyph caches are to be used?
    SDL_Texture* img;
    int i;
    int num_levels = FC_GetNumCacheLevels(font);
    for(i = 0; i < num_levels; ++i)
    {
        img = FC_GetGlyphCacheLevel(font, i);
        set_color(img, color.r, color.g, color.b, FC_GET_ALPHA(color));
    }
}

SDL_Rect FC_Draw(FC_Font* font, SDL_Renderer* dest, int x, int y, const char* formatted_text, ...)
{
    if(formatted_text == nullptr || font == nullptr)
        return {x, y, 0, 0};

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    set_color_for_all_caches(font, font->default_color);

    return FC_RenderLeft(font, dest, x, y, {1,1}, fc_buffer);
}



typedef struct FC_StringList
{
    char* value;
    struct FC_StringList* next;
} FC_StringList;

void FC_StringListFree(FC_StringList* node)
{
    // Delete the nodes in order
    while(node != nullptr)
    {
        FC_StringList* last = node;
        node = node->next;

        free(last->value);
        free(last);
    }
}

FC_StringList** FC_StringListPushBack(FC_StringList** node, char* value, Uint8 copy)
{
    if(node == nullptr)
    {
        return nullptr;
    }

    // Get to the last node
    while(*node != nullptr)
    {
        node = &(*node)->next;
    }

    *node = (FC_StringList*)malloc(sizeof(FC_StringList));

    (*node)->value = (copy? U8_strdup(value) : value);
    (*node)->next = nullptr;

    return node;
}

FC_StringList** FC_StringListPushBackBytes(FC_StringList** node, const char* data, int num_bytes)
{
    if(node == nullptr)
    {
        return node;
    }

    // Get to the last node
    while(*node != nullptr)
    {
        node = &(*node)->next;
    }

    *node = (FC_StringList*)malloc(sizeof(FC_StringList));

    (*node)->value = (char*)malloc(num_bytes + 1);
    memcpy((*node)->value, data, num_bytes);
    (*node)->value[num_bytes] = '\0';
    (*node)->next = nullptr;

    return node;
}

static FC_StringList* FC_Explode(const char* text, char delimiter)
{
    FC_StringList* head;
    FC_StringList* new_node;
    FC_StringList** node;
    const char* start;
    const char* end;
    unsigned int size;
    if(text == nullptr)
        return nullptr;

    head = nullptr;
    node = &head;

    // Doesn't technically support UTF-8, but it's probably fine, right?
    size = 0;
    start = end = text;
    while(1)
    {
        if(*end == delimiter || *end == '\0')
        {
            *node = (FC_StringList*)malloc(sizeof(FC_StringList));
            new_node = *node;

            new_node->value = (char*)malloc(size + 1);
            memcpy(new_node->value, start, size);
            new_node->value[size] = '\0';

            new_node->next = nullptr;

            if(*end == '\0')
                break;

            node = &((*node)->next);
            start = end+1;
            size = 0;
        }
        else
            ++size;

        ++end;
    }

    return head;
}

static FC_StringList* FC_ExplodeBreakingSpace(const char* text, FC_StringList** spaces)
{
    FC_StringList* head;
    FC_StringList** node;
    const char* start;
    const char* end;
    unsigned int size;
    if(text == nullptr)
        return nullptr;

    head = nullptr;
    node = &head;

    // Warning: spaces must not be initialized before this function
    *spaces = nullptr;

    // Doesn't technically support UTF-8, but it's probably fine, right?
    size = 0;
    start = end = text;
    while(1)
    {
        // Add any characters here that should make separate words (except for \n?)
        if(*end == ' ' || *end == '\t' || *end == '\0')
        {
            FC_StringListPushBackBytes(node, start, size);
            FC_StringListPushBackBytes(spaces, end, 1);

            if(*end == '\0')
                break;

            node = &((*node)->next);
            start = end+1;
            size = 0;
        }
        else
            ++size;

        ++end;
    }

    return head;
}

static FC_StringList* FC_ExplodeAndKeep(const char* text, char delimiter)
{
    FC_StringList* head;
    FC_StringList** node;
    const char* start;
    const char* end;
    unsigned int size;
    if(text == nullptr)
        return nullptr;

    head = nullptr;
    node = &head;

    // Doesn't technically support UTF-8, but it's probably fine, right?
    size = 0;
    start = end = text;
    while(1)
    {
        if(*end == delimiter || *end == '\0')
        {
            FC_StringListPushBackBytes(node, start, size);

            if(*end == '\0')
                break;

            node = &((*node)->next);
            start = end;
            size = 1;
        }
        else
            ++size;

        ++end;
    }

    return head;
}

static void FC_RenderAlign(FC_Font* font, SDL_Renderer* dest, int x, int y, int width, FC_Scale scale, FC_AlignEnum align, const char* text)
{
    switch(align)
    {
        case FC_ALIGN_LEFT:
            FC_RenderLeft(font, dest, x, y, scale, text);
            break;
        case FC_ALIGN_CENTER:
            FC_RenderCenter(font, dest, x + width/2, y, scale, text);
            break;
        case FC_ALIGN_RIGHT:
            FC_RenderRight(font, dest, x + width, y, scale, text);
            break;
    }
}

static FC_StringList* FC_GetBufferFitToColumn(FC_Font* font, int width, FC_Scale scale, Uint8 keep_newlines)
{
    FC_StringList* result = nullptr;
    FC_StringList** current = &result;

    FC_StringList *ls, *iter;

    ls = (keep_newlines? FC_ExplodeAndKeep(fc_buffer, '\n') : FC_Explode(fc_buffer, '\n'));
    for(iter = ls; iter != nullptr; iter = iter->next)
    {
        char* line = iter->value;

        // If line is too long, then add words one at a time until we go over.
        if(width > 0 && FC_GetWidth(font, "%s", line) > width)
        {
            FC_StringList *words, *word_iter, *spaces, *spaces_iter;

            words = FC_ExplodeBreakingSpace(line, &spaces);
            // Skip the first word for the iterator, so there will always be at least one word per line
            line = new_concat(words->value, spaces->value);
            for(word_iter = words->next, spaces_iter = spaces->next; word_iter != nullptr && spaces_iter != nullptr; word_iter = word_iter->next, spaces_iter = spaces_iter->next)
            {
                char* line_plus_word = new_concat(line, word_iter->value);
                char* word_plus_space = new_concat(word_iter->value, spaces_iter->value);
                if(FC_GetWidth(font, "%s", line_plus_word) > width)
                {
                    current = FC_StringListPushBack(current, line, 0);

                    line = word_plus_space;
                }
                else
                {
                    replace_concat(&line, word_plus_space);
                    free(word_plus_space);
                }
                free(line_plus_word);
            }
            current = FC_StringListPushBack(current, line, 0);
            FC_StringListFree(words);
            FC_StringListFree(spaces);
        }
        else
        {
            current = FC_StringListPushBack(current, line, 0);
            iter->value = nullptr;
        }
    }
    FC_StringListFree(ls);

    return result;
}

static void FC_DrawColumnFromBuffer(FC_Font* font, SDL_Renderer* dest, SDL_Rect box, int* total_height, FC_Scale scale, FC_AlignEnum align)
{
    int y = box.y;
    FC_StringList *ls, *iter;

    ls = FC_GetBufferFitToColumn(font, box.w, scale, 0);
    for(iter = ls; iter != nullptr; iter = iter->next)
    {
        FC_RenderAlign(font, dest, box.x, y, box.w, scale, align, iter->value);
        y += FC_GetLineHeight(font);
    }
    FC_StringListFree(ls);

    if(total_height != nullptr)
        *total_height = y - box.y;
}

SDL_Rect FC_DrawBox(FC_Font* font, SDL_Renderer* dest, SDL_Rect box, const char* formatted_text, ...)
{
    Uint8 useClip;
    if(formatted_text == nullptr || font == nullptr)
        return {box.x, box.y, 0, 0};

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    useClip = has_clip(dest);
    SDL_Rect oldclip, newclip;
    if(useClip)
    {
        oldclip = get_clip(dest);
        newclip = SDL_RectIntersect(oldclip, box);
    }
    else
        newclip = box;

    set_clip(dest, &newclip);

    set_color_for_all_caches(font, font->default_color);

    FC_DrawColumnFromBuffer(font, dest, box, nullptr, {1,1}, FC_ALIGN_LEFT);

    if(useClip)
        set_clip(dest, &oldclip);
    else
        set_clip(dest, nullptr);

    return box;
}

SDL_Rect FC_DrawBoxAlign(FC_Font* font, SDL_Renderer* dest, SDL_Rect box, FC_AlignEnum align, const char* formatted_text, ...)
{
    Uint8 useClip;
    if(formatted_text == nullptr || font == nullptr)
        return {box.x, box.y, 0, 0};

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    useClip = has_clip(dest);
    SDL_Rect oldclip, newclip;
    if(useClip)
    {
        oldclip = get_clip(dest);
        newclip = SDL_RectIntersect(oldclip, box);
    }
    else
        newclip = box;
    set_clip(dest, &newclip);

    set_color_for_all_caches(font, font->default_color);

    FC_DrawColumnFromBuffer(font, dest, box, nullptr, {1,1}, align);

    if(useClip)
        set_clip(dest, &oldclip);
    else
        set_clip(dest, nullptr);

    return box;
}

SDL_Rect FC_DrawBoxScale(FC_Font* font, SDL_Renderer* dest, SDL_Rect box, FC_Scale scale, const char* formatted_text, ...)
{
    Uint8 useClip;
    if(formatted_text == nullptr || font == nullptr)
        return {box.x, box.y, 0, 0};

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    useClip = has_clip(dest);
    SDL_Rect oldclip, newclip;
    if(useClip)
    {
        oldclip = get_clip(dest);
        newclip = SDL_RectIntersect(oldclip, box);
    }
    else
        newclip = box;
    set_clip(dest, &newclip);

    set_color_for_all_caches(font, font->default_color);

    FC_DrawColumnFromBuffer(font, dest, box, nullptr, scale, FC_ALIGN_LEFT);

    if(useClip)
        set_clip(dest, &oldclip);
    else
        set_clip(dest, nullptr);

    return box;
}

SDL_Rect FC_DrawBoxColor(FC_Font* font, SDL_Renderer* dest, SDL_Rect box, SDL_Color color, const char* formatted_text, ...)
{
    Uint8 useClip;
    if(formatted_text == nullptr || font == nullptr)
        return {box.x, box.y, 0, 0};

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    useClip = has_clip(dest);
    SDL_Rect oldclip, newclip;
    if(useClip)
    {
        oldclip = get_clip(dest);
        newclip = SDL_RectIntersect(oldclip, box);
    }
    else
        newclip = box;
    set_clip(dest, &newclip);

    set_color_for_all_caches(font, color);

    FC_DrawColumnFromBuffer(font, dest, box, nullptr, {1,1}, FC_ALIGN_LEFT);

    if(useClip)
        set_clip(dest, &oldclip);
    else
        set_clip(dest, nullptr);

    return box;
}

SDL_Rect FC_DrawBoxEffect(FC_Font* font, SDL_Renderer* dest, SDL_Rect box, FC_Effect effect, const char* formatted_text, ...)
{
    Uint8 useClip;
    if(formatted_text == nullptr || font == nullptr)
        return {box.x, box.y, 0, 0};

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    useClip = has_clip(dest);
    SDL_Rect oldclip, newclip;
    if(useClip)
    {
        oldclip = get_clip(dest);
        newclip = SDL_RectIntersect(oldclip, box);
    }
    else
        newclip = box;
    set_clip(dest, &newclip);

    set_color_for_all_caches(font, effect.color);

    FC_DrawColumnFromBuffer(font, dest, box, nullptr, effect.scale, effect.alignment);

    if(useClip)
        set_clip(dest, &oldclip);
    else
        set_clip(dest, nullptr);

    return box;
}

SDL_Rect FC_DrawColumn(FC_Font* font, SDL_Renderer* dest, int x, int y, Uint16 width, const char* formatted_text, ...)
{
    SDL_Rect box = {x, y, width, 0};
    int total_height;

    if(formatted_text == nullptr || font == nullptr)
        return {x, y, 0, 0};

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    set_color_for_all_caches(font, font->default_color);

    FC_DrawColumnFromBuffer(font, dest, box, &total_height, {1,1}, FC_ALIGN_LEFT);

    return {box.x, box.y, width, total_height};
}

SDL_Rect FC_DrawColumnAlign(FC_Font* font, SDL_Renderer* dest, int x, int y, Uint16 width, FC_AlignEnum align, const char* formatted_text, ...)
{
    SDL_Rect box = {x, y, width, 0};
    int total_height;

    if(formatted_text == nullptr || font == nullptr)
        return {x, y, 0, 0};

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    set_color_for_all_caches(font, font->default_color);

    switch(align)
    {
    case FC_ALIGN_CENTER:
        box.x -= width/2;
        break;
    case FC_ALIGN_RIGHT:
        box.x -= width;
        break;
    default:
        break;
    }

    FC_DrawColumnFromBuffer(font, dest, box, &total_height, {1,1}, align);

    return {box.x, box.y, width, total_height};
}

SDL_Rect FC_DrawColumnScale(FC_Font* font, SDL_Renderer* dest, int x, int y, Uint16 width, FC_Scale scale, const char* formatted_text, ...)
{
    SDL_Rect box = {x, y, width, 0};
    int total_height;

    if(formatted_text == nullptr || font == nullptr)
        return {x, y, 0, 0};

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    set_color_for_all_caches(font, font->default_color);

    FC_DrawColumnFromBuffer(font, dest, box, &total_height, scale, FC_ALIGN_LEFT);

    return {box.x, box.y, width, total_height};
}

SDL_Rect FC_DrawColumnColor(FC_Font* font, SDL_Renderer* dest, int x, int y, Uint16 width, SDL_Color color, const char* formatted_text, ...)
{
    SDL_Rect box = {x, y, width, 0};
    int total_height;

    if(formatted_text == nullptr || font == nullptr)
        return {x, y, 0, 0};

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    set_color_for_all_caches(font, color);

    FC_DrawColumnFromBuffer(font, dest, box, &total_height, {1,1}, FC_ALIGN_LEFT);

    return {box.x, box.y, width, total_height};
}

SDL_Rect FC_DrawColumnEffect(FC_Font* font, SDL_Renderer* dest, int x, int y, Uint16 width, FC_Effect effect, const char* formatted_text, ...)
{
    SDL_Rect box = {x, y, width, 0};
    int total_height;

    if(formatted_text == nullptr || font == nullptr)
        return {x, y, 0, 0};

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    set_color_for_all_caches(font, effect.color);

    switch(effect.alignment)
    {
    case FC_ALIGN_CENTER:
        box.x -= width/2;
        break;
    case FC_ALIGN_RIGHT:
        box.x -= width;
        break;
    default:
        break;
    }

    FC_DrawColumnFromBuffer(font, dest, box, &total_height, effect.scale, effect.alignment);

    return {box.x, box.y, width, total_height};
}

static SDL_Rect FC_RenderCenter(FC_Font* font, SDL_Renderer* dest, int x, int y, FC_Scale scale, const char* text)
{
    SDL_Rect result = {x, y, 0, 0};
    if(text == nullptr || font == nullptr)
        return result;

    char* str = U8_strdup(text);
    char* del = str;
    char* c;

    // Go through str, when you find a \n, replace it with \0 and print it
    // then move down, back, and continue.
    for(c = str; *c != '\0';)
    {
        if(*c == '\n')
        {
            *c = '\0';
            result = SDL_RectUnion(FC_RenderLeft(font, dest, x - scale.x*FC_GetWidth(font, "%s", str)/2, y, scale, str), result);
            *c = '\n';
            c++;
            str = c;
            y += scale.y*font->height;
        }
        else
            c++;
    }

    result = SDL_RectUnion(FC_RenderLeft(font, dest, x - scale.x*FC_GetWidth(font, "%s", str)/2, y, scale, str), result);

    free(del);
    return result;
}

static SDL_Rect FC_RenderRight(FC_Font* font, SDL_Renderer* dest, int x, int y, FC_Scale scale, const char* text)
{
    SDL_Rect result = {x, y, 0, 0};
    if(text == nullptr || font == nullptr)
        return result;

    char* str = U8_strdup(text);
    char* del = str;
    char* c;

    for(c = str; *c != '\0';)
    {
        if(*c == '\n')
        {
            *c = '\0';
            result = SDL_RectUnion(FC_RenderLeft(font, dest, x - scale.x*FC_GetWidth(font, "%s", str), y, scale, str), result);
            *c = '\n';
            c++;
            str = c;
            y += scale.y*font->height;
        }
        else
            c++;
    }

    result = SDL_RectUnion(FC_RenderLeft(font, dest, x - scale.x*FC_GetWidth(font, "%s", str), y, scale, str), result);

    free(del);
    return result;
}



SDL_Rect FC_DrawScale(FC_Font* font, SDL_Renderer* dest, int x, int y, FC_Scale scale, const char* formatted_text, ...)
{
    if(formatted_text == nullptr || font == nullptr)
        return {x, y, 0, 0};

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    set_color_for_all_caches(font, font->default_color);

    return FC_RenderLeft(font, dest, x, y, scale, fc_buffer);
}

SDL_Rect FC_DrawAlign(FC_Font* font, SDL_Renderer* dest, int x, int y, FC_AlignEnum align, const char* formatted_text, ...)
{
    if(formatted_text == nullptr || font == nullptr)
        return {x, y, 0, 0};

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    set_color_for_all_caches(font, font->default_color);

    SDL_Rect result;
    switch(align)
    {
        case FC_ALIGN_LEFT:
            result = FC_RenderLeft(font, dest, x, y, {1,1}, fc_buffer);
            break;
        case FC_ALIGN_CENTER:
            result = FC_RenderCenter(font, dest, x, y, {1,1}, fc_buffer);
            break;
        case FC_ALIGN_RIGHT:
            result = FC_RenderRight(font, dest, x, y, {1,1}, fc_buffer);
            break;
        default:
            result = {x, y, 0, 0};
            break;
    }

    return result;
}

SDL_Rect FC_DrawColor(FC_Font* font, SDL_Renderer* dest, int x, int y, SDL_Color color, const char* formatted_text, ...)
{
    if(formatted_text == nullptr || font == nullptr)
        return {x, y, 0, 0};

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    set_color_for_all_caches(font, color);

    return FC_RenderLeft(font, dest, x, y, {1,1}, fc_buffer);
}


SDL_Rect FC_DrawEffect(FC_Font* font, SDL_Renderer* dest, int x, int y, FC_Effect effect, const char* formatted_text, ...)
{
    if(formatted_text == nullptr || font == nullptr)
        return {x, y, 0, 0};

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    set_color_for_all_caches(font, effect.color);

    SDL_Rect result;
    switch(effect.alignment)
    {
        case FC_ALIGN_LEFT:
            result = FC_RenderLeft(font, dest, x, y, effect.scale, fc_buffer);
            break;
        case FC_ALIGN_CENTER:
            result = FC_RenderCenter(font, dest, x, y, effect.scale, fc_buffer);
            break;
        case FC_ALIGN_RIGHT:
            result = FC_RenderRight(font, dest, x, y, effect.scale, fc_buffer);
            break;
        default:
            result = {x, y, 0, 0};
            break;
    }

    return result;
}




// Getters


FC_FilterEnum FC_GetFilterMode(FC_Font* font)
{
    if(font == nullptr)
        return FC_FILTER_NEAREST;

    return font->filter;
}

Uint16 FC_GetLineHeight(FC_Font* font)
{
    if(font == nullptr)
        return 0;

    return font->height;
}

Uint16 FC_GetHeight(FC_Font* font, const char* formatted_text, ...)
{
    if(formatted_text == nullptr || font == nullptr)
        return 0;

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    Uint16 numLines = 1;
    const char* c;

    for (c = fc_buffer; *c != '\0'; c++)
    {
        if(*c == '\n')
            numLines++;
    }

    //   Actual height of letter region + line spacing
    return font->height*numLines + font->lineSpacing*(numLines - 1);  //height*numLines;
}

Uint16 FC_GetWidth(FC_Font* font, const char* formatted_text, ...)
{
    if(formatted_text == nullptr || font == nullptr)
        return 0;

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    const char* c;
    Uint16 width = 0;
    Uint16 bigWidth = 0;  // Allows for multi-line strings

    for (c = fc_buffer; *c != '\0'; c++)
    {
        if(*c == '\n')
        {
            bigWidth = bigWidth >= width? bigWidth : width;
            width = 0;
            continue;
        }

        FC_GlyphData glyph;
        Uint32 codepoint = FC_GetCodepointFromUTF8(&c, 1);
        if(FC_GetGlyphData(font, &glyph, codepoint) || FC_GetGlyphData(font, &glyph, ' '))
            width += glyph.rect.w;
    }
    bigWidth = bigWidth >= width? bigWidth : width;

    return bigWidth;
}

// If width == -1, use no width limit
SDL_Rect FC_GetCharacterOffset(FC_Font* font, Uint16 position_index, int column_width, const char* formatted_text, ...)
{
    SDL_Rect result = {0, 0, 1, FC_GetLineHeight(font)};
    FC_StringList *ls, *iter;
    int num_lines = 0;
    Uint8 done = 0;

    if(formatted_text == nullptr || column_width == 0 || position_index == 0 || font == nullptr)
        return result;

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    ls = FC_GetBufferFitToColumn(font, column_width, {1,1}, 1);
    for(iter = ls; iter != nullptr;)
    {
        char* line;
        int i = 0;
        FC_StringList* next_iter = iter->next;

        ++num_lines;
        for(line = iter->value; line != nullptr && *line != '\0'; line = (char*)U8_next(line))
        {
            ++i;
            --position_index;
            if(position_index == 0)
            {
                // FIXME: Doesn't handle box-wrapped newlines correctly
                line = (char*)U8_next(line);
                line[0] = '\0';
                result.x = FC_GetWidth(font, "%s", iter->value);
                done = 1;
                break;
            }
        }
        if(done)
            break;

        // Prevent line wrapping if there are no more lines
        if(next_iter == nullptr && !done)
            result.x = FC_GetWidth(font, "%s", iter->value);
        iter = next_iter;
    }
    FC_StringListFree(ls);

    if(num_lines > 1)
    {
        result.y = (num_lines - 1) * FC_GetLineHeight(font);
    }

    return result;
}


Uint16 FC_GetColumnHeight(FC_Font* font, Uint16 width, const char* formatted_text, ...)
{
    int y = 0;

    FC_StringList *ls, *iter;

    if(font == nullptr)
        return 0;

    if(formatted_text == nullptr || width == 0)
        return font->height;

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    ls = FC_GetBufferFitToColumn(font, width, {1,1}, 0);
    for(iter = ls; iter != nullptr; iter = iter->next)
    {
        y += FC_GetLineHeight(font);
    }
    FC_StringListFree(ls);

    return y;
}

static int FC_GetAscentFromCodepoint(FC_Font* font, Uint32 codepoint)
{
    FC_GlyphData glyph;

    if(font == nullptr)
        return 0;

    // FIXME: Store ascent so we can return it here
    FC_GetGlyphData(font, &glyph, codepoint);
    return glyph.rect.h;
}

static int FC_GetDescentFromCodepoint(FC_Font* font, Uint32 codepoint)
{
    FC_GlyphData glyph;

    if(font == nullptr)
        return 0;

    // FIXME: Store descent so we can return it here
    FC_GetGlyphData(font, &glyph, codepoint);
    return glyph.rect.h;
}

int FC_GetAscent(FC_Font* font, const char* formatted_text, ...)
{
    Uint32 codepoint;
    int max, ascent;
    const char* c;

    if(font == nullptr)
        return 0;

    if(formatted_text == nullptr)
        return font->ascent;

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    max = 0;
    c = fc_buffer;

    while(*c != '\0')
    {
        codepoint = FC_GetCodepointFromUTF8(&c, 1);
        if(codepoint != 0)
        {
            ascent = FC_GetAscentFromCodepoint(font, codepoint);
            if(ascent > max)
                max = ascent;
        }
        ++c;
    }
    return max;
}

int FC_GetDescent(FC_Font* font, const char* formatted_text, ...)
{
    Uint32 codepoint;
    int max, descent;
    const char* c;

    if(font == nullptr)
        return 0;

    if(formatted_text == nullptr)
        return font->descent;

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    max = 0;
    c = fc_buffer;

    while(*c != '\0')
    {
        codepoint = FC_GetCodepointFromUTF8(&c, 1);
        if(codepoint != 0)
        {
            descent = FC_GetDescentFromCodepoint(font, codepoint);
            if(descent > max)
                max = descent;
        }
        ++c;
    }
    return max;
}

int FC_GetBaseline(FC_Font* font)
{
    if(font == nullptr)
        return 0;

    return font->baseline;
}

int FC_GetSpacing(FC_Font* font)
{
    if(font == nullptr)
        return 0;

    return font->letterSpacing;
}

int FC_GetLineSpacing(FC_Font* font)
{
    if(font == nullptr)
        return 0;

    return font->lineSpacing;
}

Uint16 FC_GetMaxWidth(FC_Font* font)
{
    if(font == nullptr)
        return 0;

    return font->maxWidth;
}

SDL_Color FC_GetDefaultColor(FC_Font* font)
{
    if(font == nullptr)
    {
        SDL_Color c = {0,0,0,255};
        return c;
    }

    return font->default_color;
}

SDL_Rect FC_GetBounds(FC_Font* font, int x, int y, FC_AlignEnum align, FC_Scale scale, const char* formatted_text, ...)
{
    SDL_Rect result = {x, y, 0, 0};

    if(formatted_text == nullptr)
        return result;

    // Create a temp buffer while GetWidth and GetHeight use fc_buffer.
    char* temp = (char*)malloc(fc_buffer_size);
    FC_EXTRACT_VARARGS(temp, formatted_text);

    result.w = FC_GetWidth(font, "%s", temp) * scale.x;
    result.h = FC_GetHeight(font, "%s", temp) * scale.y;

    switch(align)
    {
        case FC_ALIGN_LEFT:
            break;
        case FC_ALIGN_CENTER:
            result.x -= result.w/2;
            break;
        case FC_ALIGN_RIGHT:
            result.x -= result.w;
            break;
        default:
            break;
    }

    free(temp);

    return result;
}

Uint8 FC_InRect(int x, int y, SDL_Rect input_rect)
{
    return (input_rect.x <= x && x <= input_rect.x + input_rect.w && input_rect.y <= y && y <= input_rect.y + input_rect.h);
}

// TODO: Make it work with alignment
Uint16 FC_GetPositionFromOffset(FC_Font* font, int x, int y, int column_width, FC_AlignEnum align, const char* formatted_text, ...)
{
    FC_StringList *ls, *iter;
    Uint8 done = 0;
    int height = FC_GetLineHeight(font);
    Uint16 position = 0;
    int current_x = 0;
    int current_y = 0;
    FC_GlyphData glyph_data;

    if(formatted_text == nullptr || column_width == 0 || font == nullptr)
        return 0;

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    ls = FC_GetBufferFitToColumn(font, column_width, {1,1}, 1);
    for(iter = ls; iter != nullptr; iter = iter->next)
    {
        char* line;

        for(line = iter->value; line != nullptr && *line != '\0'; line = (char*)U8_next(line))
        {
            if(FC_GetGlyphData(font, &glyph_data, FC_GetCodepointFromUTF8((const char**)&line, 0)))
            {
                if(FC_InRect(x, y, {current_x, current_y, glyph_data.rect.w, glyph_data.rect.h}))
                {
                    done = 1;
                    break;
                }

                current_x += glyph_data.rect.w;
            }
            position++;
        }
        if(done)
            break;

        current_x = 0;
        current_y += height;
        if(y < current_y)
            break;
    }
    FC_StringListFree(ls);

    return position;
}

int FC_GetWrappedText(FC_Font* font, char* result, int max_result_size, Uint16 width, const char* formatted_text, ...)
{
    FC_StringList *ls, *iter;

    if(font == nullptr)
        return 0;

    if(formatted_text == nullptr || width == 0)
        return 0;

    FC_EXTRACT_VARARGS(fc_buffer, formatted_text);

    ls = FC_GetBufferFitToColumn(font, width, {1,1}, 0);
    int size_so_far = 0;
    int size_remaining = max_result_size-1; // reserve for \0
    for(iter = ls; iter != nullptr && size_remaining > 0; iter = iter->next)
    {
        // Copy as much of this line as we can
        int len = strlen(iter->value);
        int num_bytes = FC_MIN(len, size_remaining);
        memcpy(&result[size_so_far], iter->value, num_bytes);
        size_so_far += num_bytes;

        // If there's another line, add newline character
        if(size_remaining > 0 && iter->next != nullptr)
        {
            --size_remaining;
            result[size_so_far] = '\n';
            ++size_so_far;
        }
    }
    FC_StringListFree(ls);

    result[size_so_far] = '\0';

    return size_so_far;
}



// Setters


void FC_SetFilterMode(FC_Font* font, FC_FilterEnum filter)
{
    if(font == nullptr)
        return;

    if(font->filter != filter)
    {
        font->filter = filter;
    }
}


void FC_SetSpacing(FC_Font* font, int LetterSpacing)
{
    if(font == nullptr)
        return;

    font->letterSpacing = LetterSpacing;
}

void FC_SetLineSpacing(FC_Font* font, int LineSpacing)
{
    if(font == nullptr)
        return;

    font->lineSpacing = LineSpacing;
}

void FC_SetDefaultColor(FC_Font* font, SDL_Color color)
{
    if(font == nullptr)
        return;

    font->default_color = color;
}
