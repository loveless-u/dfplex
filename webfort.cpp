//
//  webfort.cpp
//  Web Fortress
//
//  Created by Vitaly Pronkin on 14/05/14.
//  Copyright (c) 2014 mifki. All rights reserved.
//

#include <stdint.h>
#include <iostream>
#include <map>
#include <vector>
#include "Core.h"
#include "Console.h"
#include "Export.h"
#include "PluginManager.h"
#include "VTableInterpose.h"
#include "modules/Maps.h"
#include "modules/World.h"
#include "modules/MapCache.h"
#include "modules/Gui.h"
#include "modules/Screen.h"
#include "modules/Buildings.h"
#include "MemAccess.h"
#include "VersionInfo.h"
#include "df/construction.h"
#include "df/block_square_event_frozen_liquidst.h"
#include "df/graphic.h"
#include "df/enabler.h"
#include "df/renderer.h"
#include "df/building.h"
#include "df/building_type.h"
#include "df/buildings_other_id.h"
#include "df/item.h"
#include "df/item_type.h"
#include "df/items_other_id.h"
#include "df/tiletype.h"
#include "df/viewscreen_dwarfmodest.h"
#include "df/viewscreen_setupadventurest.h"
#include "df/viewscreen_dungeonmodest.h"
#include "df/viewscreen_choose_start_sitest.h"
#include "df/viewscreen_new_regionst.h"
#include "df/viewscreen_layer_export_play_mapst.h"
#include "df/viewscreen_layer_world_gen_paramst.h"
#include "df/viewscreen_overallstatusst.h"
#include "df/viewscreen_tradegoodsst.h"
#include "df/viewscreen_petst.h"
#include "df/viewscreen_movieplayerst.h"
#include "df/ui_sidebar_mode.h"
#include "df/init.h"
#include "df/init_display.h"
#include "df/init_display_flags.h"

#ifdef WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
#elif defined(__APPLE__)
#else
    #include <dlfcn.h>
#endif

#include "tinythread.h"

#include "SDL_events.h"
#include "SDL_keysym.h"
extern "C"
{
extern int SDL_PushEvent( SDL::Event* event );
}

#include <nopoll.h>

#define PLAYTIME 60*15
#define IDLETIME 60*3

typedef float GLfloat;
typedef unsigned int GLuint;

using df::global::world;
using std::string;
using std::vector;
using df::global::enabler;
using df::global::gps;
using df::global::ui;
using df::global::init;

struct texture_fullid {
    int texpos;
    float r, g, b;
    float br, bg, bb;
};

struct gl_texpos {
    GLfloat left, right, top, bottom;
};

vector<string> split(const char *str, char c = ' ')
{
    vector<string> result;

    do
    {
        const char *begin = str;

        while(*str != c && *str)
            str++;

        result.push_back(string(begin, str));
    } while (0 != *str++);

    return result;
}

DFHACK_PLUGIN("webfort");

void (*load_multi_pdim)(void *tex,const string &filename, long *tex_pos, long dimx, long dimy, bool convert_magenta, long *disp_x, long *disp_y);
void (*update_tile_old)(df::renderer *r, int x, int y);
void (*render_old)(df::renderer *r);

#ifdef WIN32
__declspec(naked) void load_multi_pdim_x(void *tex, const string &filename, long *tex_pos, long dimx, long dimy, bool convert_magenta, long *disp_x, long *disp_y)
{
    __asm {
        push ebp
        mov ebp, esp

        push disp_y
        push disp_x
        push 10h
        push 10h
        mov ecx, tex_pos
        push tex
        mov edx, filename

        call load_multi_pdim

        mov esp, ebp
        pop ebp
        ret
    }    
}
#else
#define load_multi_pdim_x load_multi_pdim
#endif        

#ifdef WIN32
__declspec(naked) void render_old_x(df::renderer *r)
{
    __asm {
        push ebp
        mov ebp, esp

        mov ecx, r
        call render_old

        mov esp, ebp
        pop ebp
        ret
    }
}    
#else
#define render_old_x render_old
#endif    

#ifdef WIN32
__declspec(naked) void update_tile_old_x(df::renderer *r, int x, int y)
{
    __asm {
        push ebp
        mov ebp, esp

        push y
        push x

        mov ecx, r
        call update_tile_old

        mov esp, ebp
        pop ebp
        ret
    }   
} 
#else
#define update_tile_old_x update_tile_old
#endif    

struct tileset {
    string small_font_path;
    string large_font_path;
    long small_texpos[16*16], large_texpos[16*16];
};

struct tileref {
    int tilesetidx;
    int tile;
};

struct override {
    bool building;
    bool tiletype;
    int id, type, subtype;
    struct tileref newtile;
};

typedef struct {
    noPollConn *conn;
    bool active;
    unsigned char mod[256*256];
    time_t itime;
    time_t atime;
} Client;

static vector< Client* > clients;
static int activeidx = -1;

static tthread::thread * wsthread;
static void wsthreadmain(void*);

static vector< struct tileset > tilesets;

static bool enabled, texloaded;
static bool has_textfont, has_overrides;
static color_ostream *out2;
static vector< struct override > *overrides[256];
static struct tileref override_defs[256];
static df::item_flags bad_item_flags;

static unsigned char sc[256*256*5];
static unsigned char buf[64*1024];

static int newwidth, newheight;
static volatile bool needsresize, shownextturn;


bool is_text_tile(int x, int y, bool &is_map)
{
    const int tile = x * gps->dimy + y;
    df::viewscreen * ws = Gui::getCurViewscreen();

    int32_t w = gps->dimx, h = gps->dimy;

    is_map = false;

    if (!x || !y || x == w - 1 || y == h - 1)
       return has_textfont;

#define IS_SCREEN(_sc) df::_sc::_identity.is_direct_instance(ws) 

    if (IS_SCREEN(viewscreen_dwarfmodest))
    {
        uint8_t menu_width, area_map_width;
        Gui::getMenuWidth(menu_width, area_map_width);
        int32_t menu_left = w - 1, menu_right = w - 1;

        bool menuforced = (ui->main.mode != df::ui_sidebar_mode::Default || df::global::cursor->x != -30000);

        if ((menuforced || menu_width == 1) && area_map_width == 2) // Menu + area map
        {
            menu_left = w - 56;
            menu_right = w - 25;
        }
        else if (menu_width == 2 && area_map_width == 2) // Area map only
        {
            menu_left = menu_right = w - 25;
        }
        else if (menu_width == 1) // Wide menu
            menu_left = w - 56;
        else if (menuforced || (menu_width == 2 && area_map_width == 3)) // Menu only
            menu_left = w - 32; 

        if (x >= menu_left && x <= menu_right)
        {
            if (menuforced && ui->main.mode == df::ui_sidebar_mode::Burrows && ui->burrows.in_define_mode)
            {
                // Make burrow symbols use graphics font
                if ((y != 12 && y != 13 && !(x == menu_left + 2 && y == 2)) || x == menu_left || x == menu_right) 
                    return has_textfont;
            }
            else
                return has_textfont;
        }

        is_map = (x > 0 && x < menu_left);

        return false;
    }

    if (!has_textfont)
        return false;    

    if (IS_SCREEN(viewscreen_setupadventurest))
    {
        df::viewscreen_setupadventurest *s = static_cast<df::viewscreen_setupadventurest*>(ws);
        if (s->subscreen != df::viewscreen_setupadventurest::Nemesis)
            return true;
        else if (x < 58 || x >= 78 || y == 0 || y >= 21)
            return true;

        return false;
    }
    
    if (IS_SCREEN(viewscreen_dungeonmodest))
    {
        //df::viewscreen_dungeonmodest *s = strict_virtual_cast<df::viewscreen_dungeonmodest>(ws);
        //TODO

        if (y >= h-2)
            return true;

        return false;
    }

    if (IS_SCREEN(viewscreen_choose_start_sitest))
    {
        if (y <= 1 || y >= h - 6 || x == 0 || x >= 57)
            return true;

        return false;
    }
        
    if (IS_SCREEN(viewscreen_new_regionst))
    {
        if (y <= 1 || y >= h - 2 || x <= 37 || x == w - 1)
            return true;

        return false;
    }

    if (IS_SCREEN(viewscreen_layer_export_play_mapst))
    {
        if (x == w - 1 || x < w - 23)
            return true;

        return false;
    }

    if (IS_SCREEN(viewscreen_overallstatusst))
    {
        if ((x == 46 || x == 71) && y >= 8)
            return false;

        return true;
    }

    if (IS_SCREEN(viewscreen_movieplayerst))
    {
        df::viewscreen_movieplayerst *s = static_cast<df::viewscreen_movieplayerst*>(ws);
        return !s->is_playing;
    }    

    /*if (IS_SCREEN(viewscreen_petst))
    {
        if (x == 41 && y >= 7)
            return false;

        return true;
    }*/

    //*out2 << Core::getInstance().p->readClassName(*(void**)ws) << std::endl;

    return true;
}

void screen_to_texid2(df::renderer *r, int x, int y, struct texture_fullid &ret) {
    const int tile = x * gps->dimy + y;
    const unsigned char *s = r->screen + tile*4;

    int ch;
    int bold;
    int fg;
    int bg;

    ch   = s[0];
    bold = (s[3] != 0) * 8;
    fg   = (s[1] + bold) % 16;
    bg   = s[2] % 16;
  
    const long texpos             = r->screentexpos[tile];
    const char addcolor           = r->screentexpos_addcolor[tile];
    const unsigned char grayscale = r->screentexpos_grayscale[tile];
    const unsigned char cf        = r->screentexpos_cf[tile];
    const unsigned char cbr       = r->screentexpos_cbr[tile];

    if (texpos) {
      ret.texpos = texpos;
      if (grayscale) {
        ret.r = enabler->ccolor[cf][0];
        ret.g = enabler->ccolor[cf][1];
        ret.b = enabler->ccolor[cf][2];
        ret.br = enabler->ccolor[cbr][0];
        ret.bg = enabler->ccolor[cbr][1];
        ret.bb = enabler->ccolor[cbr][2];
      } else if (addcolor) {
        goto use_ch;
      } else {
        ret.r = ret.g = ret.b = 1;
        ret.br = ret.bg = ret.bb = 0;
      }
      return;
    }
  
  ret.texpos = enabler->fullscreen ?
    init->font.large_font_texpos[ch] :
    init->font.small_font_texpos[ch];

 use_ch:
  ret.r = enabler->ccolor[fg][0];
  ret.g = enabler->ccolor[fg][1];
  ret.b = enabler->ccolor[fg][2];
  ret.br = enabler->ccolor[bg][0];
  ret.bg = enabler->ccolor[bg][1];
  ret.bb = enabler->ccolor[bg][2];
}

void write_tile_arrays(df::renderer *r, int x, int y, GLfloat *fg, GLfloat *bg, GLfloat *tex)
{
    struct texture_fullid ret;
    screen_to_texid2(r, x, y, ret);
        
    for (int i = 0; i < 6; i++) {
        *(fg++) = ret.r;
        *(fg++) = ret.g;
        *(fg++) = ret.b;
        *(fg++) = 1;
        
        *(bg++) = ret.br;
        *(bg++) = ret.bg;
        *(bg++) = ret.bb;
        *(bg++) = 1;
    }

    const int tile = x * gps->dimy + y;
    const unsigned char *s = r->screen + tile*4;
    unsigned char *ss = sc + tile*4;    
    *(unsigned int*)ss = *(unsigned int*)s;
    
    bool is_map;
    if (is_text_tile(x, y, is_map))
    {
        ret.texpos = enabler->fullscreen ? tilesets[1].large_texpos[s[0]] : tilesets[1].small_texpos[s[0]];

        ss[2] |= 64;
    }
    else if (is_map && has_overrides)
    {
        int s0 = s[0];
        if (overrides[s0])
        {
            int xx = *df::global::window_x + x-1;
            int yy = *df::global::window_y + y-1;
            int zz = *df::global::window_z;
            bool matched = false;

            // No block - no items/buildings/tiletype
            df::map_block *block = Maps::getTileBlock(xx, yy, zz);
            if (block)
            {
                int tiletype = block->tiletype[xx&15][yy&15];

                for (int j = 0; j < overrides[s0]->size(); j++)
                {
                    struct override &o = (*overrides[s0])[j];

                    if (o.tiletype)
                    {
                        if (tiletype == o.type)
                            matched = true;
                    }
                    else if (o.building)
                    {
                        auto ilist = world->buildings.other[o.id];
                        for (auto it = ilist.begin(); it != ilist.end(); it++)
                        {
                            df::building *bld = *it;

                            if (zz != bld->z || xx < bld->x1 || xx > bld->x2 || yy < bld->y1 || yy > bld->y2)
                                continue;
                            if (o.type != -1 && bld->getType() != o.type)
                                continue;
                            if (o.subtype != -1 && bld->getSubtype() != o.subtype)
                                continue;

                            matched = true;
                            break;
                        }
                    }
                    else
                    {
                        auto ilist = world->items.other[o.id];
                        for (auto it = ilist.begin(); it != ilist.end(); it++)
                        {
                            df::item *item = *it;
                            if (!(zz == item->pos.z && xx == item->pos.x && yy == item->pos.y))
                                continue;
                            if (item->flags.whole & bad_item_flags.whole)
                                continue;
                            if (o.type != -1 && item->getType() != o.type)
                                continue;
                            if (o.subtype != -1 && item->getSubtype() != o.subtype)
                                continue;

                            matched = true;
                            break;                            
                        }
                    }

                    if (matched)
                    {
                        ret.texpos = enabler->fullscreen ?
                            tilesets[o.newtile.tilesetidx].large_texpos[o.newtile.tile] :
                            tilesets[o.newtile.tilesetidx].small_texpos[o.newtile.tile];

                        break;
                    }
                }
            }

            // Default
            if (!matched && override_defs[s0].tile)
                ret.texpos = enabler->fullscreen ?
                    tilesets[override_defs[s0].tilesetidx].large_texpos[override_defs[s0].tile] :
                    tilesets[override_defs[s0].tilesetidx].small_texpos[override_defs[s0].tile];
        }
    }

    for (int i = 0; i < clients.size(); i++)
        clients[i]->mod[tile] = 0;    
    
    // Set texture coordinates
    gl_texpos *txt = (gl_texpos*) enabler->textures.gl_texpos;
    *(tex++) = txt[ret.texpos].left;   // Upper left
    *(tex++) = txt[ret.texpos].bottom;
    *(tex++) = txt[ret.texpos].right;  // Upper right
    *(tex++) = txt[ret.texpos].bottom;
    *(tex++) = txt[ret.texpos].left;   // Lower left
    *(tex++) = txt[ret.texpos].top;
    
    *(tex++) = txt[ret.texpos].left;   // Lower left
    *(tex++) = txt[ret.texpos].top;
    *(tex++) = txt[ret.texpos].right;  // Upper right
    *(tex++) = txt[ret.texpos].bottom;
    *(tex++) = txt[ret.texpos].right;  // Lower right
    *(tex++) = txt[ret.texpos].top;
}

#ifdef WIN32
void __stdcall update_tile(int x, int y)
#else
void update_tile(df::renderer *r, int x, int y)
#endif
{
#ifdef WIN32
    df::renderer *r = enabler->renderer;
#endif

    if (!enabled || !texloaded)
    {
        update_tile_old_x(r, x, y);
        return;
    }

    GLfloat *_fg = (GLfloat*)*(GLfloat**)((char*)r+0x44);
    GLfloat *_bg = (GLfloat*)*(GLfloat**)((char*)r+0x48);
    GLfloat *_tex = (GLfloat*)*(GLfloat**)((char*)r+0x4c);

    const int tile = x*gps->dimy + y;

    GLfloat *fg  = _fg + tile * 4 * 6;
    GLfloat *bg  = _bg + tile * 4 * 6;
    GLfloat *tex = _tex + tile * 2 * 6;

    write_tile_arrays(r, x, y, fg, bg, tex);
}

#ifdef WIN32
void __stdcall render()
#else
void render(df::renderer *r)
#endif
{
#ifdef WIN32
    df::renderer *r = enabler->renderer;
#endif

    if (!texloaded)
    {
        long dx, dy;
        void *t = &enabler->textures;

        for (int j = 0; j < tilesets.size(); j++)
        {
            struct tileset &ts = tilesets[j];
            if (!ts.small_font_path.length())
                continue;

            load_multi_pdim_x(t, ts.small_font_path, tilesets[j].small_texpos, 16, 16, true, &dx, &dy);
            if (ts.large_font_path != ts.small_font_path)
                load_multi_pdim_x(t, ts.large_font_path, tilesets[j].large_texpos, 16, 16, true, &dx, &dy);
            else
                memcpy(ts.large_texpos, ts.small_texpos, sizeof(ts.large_texpos));
        }

        texloaded = true;
        gps->force_full_display_count = true;
    }

    if (needsresize)
    {
        enabler->renderer->grid_resize(newwidth,newheight);
        needsresize = false;
    }
    /*if (shownextturn)
    {
        df::popup_message *popup = new df::popup_message();
        popup->text = "Next player, unpause to continue.";
        popup->color = 1;
        popup->bright = 0;
        world->status.popups.push_back(popup);

        shownextturn = false;        
    }*/

    render_old_x(r);
}


void hook()
{
    if (enabled)
        return;

    long **rVtable = (long **)enabler->renderer;

#ifdef WIN32
    HANDLE process = ::GetCurrentProcess();
    DWORD protection = PAGE_READWRITE;
    DWORD oldProtection;
    if ( ::VirtualProtectEx( process, rVtable[0], 4*sizeof(void*), protection, &oldProtection ) )
    {
#endif

    update_tile_old = (void (*)(df::renderer *r, int x, int y))rVtable[0][0];
    rVtable[0][0] = (long)&update_tile;

    render_old = (void(*)(df::renderer *r))rVtable[0][2];
    rVtable[0][2] = (long)&render;

    enabled = true;   

#ifdef WIN32
    VirtualProtectEx( process, rVtable[0], 4*sizeof(void*), oldProtection, &oldProtection );
    }
#endif

}

void unhook()
{
    if (!enabled)
        return;

    enabled = false;

    df::renderer* renderer = enabler->renderer;
    long **rVtable = (long **)enabler->renderer;

#ifdef WIN32
    HANDLE process = ::GetCurrentProcess();
    DWORD protection = PAGE_READWRITE;
    DWORD oldProtection;
    if ( ::VirtualProtectEx( process, rVtable[0], 4*sizeof(void*), protection, &oldProtection ) )
    {
#endif
    rVtable[0][0] = (long)update_tile_old;
    rVtable[0][2] = (long)render_old;
#ifdef WIN32
    VirtualProtectEx( process, rVtable[0], 4*sizeof(void*), oldProtection, &oldProtection );
    }
#endif

    gps->force_full_display_count = true;
}

bool get_font_paths()
{
    string small_font_path, gsmall_font_path;
    string large_font_path, glarge_font_path;

    std::ifstream fseed("data/init/init.txt");
    if(fseed.is_open())
    {
        string str;

        while(std::getline(fseed,str))
        {
            size_t b = str.find("[");
            size_t e = str.rfind("]");

            if (b == string::npos || e == string::npos || str.find_first_not_of(" ") < b)
                continue;

            str = str.substr(b+1, e-1);
            vector<string> tokens = split(str.c_str(), ':');

            if (tokens.size() != 2)
                continue;
                                
            if(tokens[0] == "FONT")
            {
                small_font_path = "data/art/" + tokens[1];
                continue;
            }

            if(tokens[0] == "FULLFONT")
            {
                large_font_path = "data/art/" + tokens[1];
                continue;
            }

            if(tokens[0] == "GRAPHICS_FONT")
            {
                gsmall_font_path = "data/art/" + tokens[1];
                continue;
            }

            if(tokens[0] == "GRAPHICS_FULLFONT")
            {
                glarge_font_path = "data/art/" + tokens[1];
                continue;
            }                    
        }
    }

    fseed.close();
    
    if (!(small_font_path == gsmall_font_path && large_font_path == glarge_font_path))
    {
        struct tileset ts;
        ts.small_font_path = small_font_path;
        ts.large_font_path = large_font_path;

        tilesets.push_back(ts);
        return true;
    }
    else
    {
        struct tileset ts;
        tilesets.push_back(ts);
        return false;
    }
}

bool load_overrides()
{
    bool found = false;

    std::ifstream fseed("data/init/overrides.txt");
    if(fseed.is_open())
    {
        string str;

        while(std::getline(fseed,str))
        {
            size_t b = str.find("[");
            size_t e = str.rfind("]");

            if (b == string::npos || e == string::npos || str.find_first_not_of(" ") < b)
                continue;

            str = str.substr(b+1, e-1);
            vector<string> tokens = split(str.c_str(), ':');

            if (tokens[0] == "TILESET")
            {
                struct tileset ts;
                ts.small_font_path = "data/art/" + tokens[1];
                ts.large_font_path = "data/art/" + tokens[2];
                tilesets.push_back(ts);
                continue;
            }
            
            if (tokens[0] == "OVERRIDE")
            {
                if (tokens.size() == 6)
                {
                    int tile = atoi(tokens[1].c_str());
                    if (tokens[2] != "T")
                        continue;

                    struct override o;
                    o.tiletype = true;

                    tiletype::tiletype type;
                    if (find_enum_item(&type, tokens[3]))
                        o.type = type;
                    else
                        continue;

                    o.newtile.tilesetidx = atoi(tokens[4].c_str());
                    o.newtile.tile = atoi(tokens[5].c_str());

                    if (!overrides[tile])
                        overrides[tile] = new vector< struct override >;
                    overrides[tile]->push_back(o);
                }
                else if (tokens.size() == 8)
                {
                    int tile = atoi(tokens[1].c_str());

                    struct override o;
                    o.tiletype = false;
                    o.building = (tokens[2] == "B");
                    if (o.building)
                    {
                        buildings_other_id::buildings_other_id id;
                        if (find_enum_item(&id, tokens[3]))
                            o.id = id;
                        else
                            o.id = -1;

                        building_type::building_type type;
                        if (find_enum_item(&type, tokens[4]))
                            o.type = type;
                        else
                            o.type = -1;
                    }
                    else
                    {
                        items_other_id::items_other_id id;
                        if (find_enum_item(&id, tokens[3]))
                            o.id = id;
                        else
                            o.id = -1;

                        item_type::item_type type;
                        if (find_enum_item(&type, tokens[4]))
                            o.type = type;
                        else
                            o.type = -1;
                    }

                    if (tokens[5].length() > 0)
                        o.subtype = atoi(tokens[5].c_str());
                    else
                        o.subtype = -1;

                    o.newtile.tilesetidx = atoi(tokens[6].c_str());
                    o.newtile.tile = atoi(tokens[7].c_str());

                    if (!overrides[tile])
                        overrides[tile] = new vector< struct override >;
                    overrides[tile]->push_back(o);
                }
                else if (tokens.size() == 4)
                {
                    int tile = atoi(tokens[1].c_str());
                    override_defs[tile].tilesetidx = atoi(tokens[2].c_str());
                    override_defs[tile].tile = atoi(tokens[3].c_str());
                }

                found = true;
                continue;
            }
        }
    }

    fseed.close();
    return found;
}

#ifdef __APPLE__
//0x0079cb2a+4 0x14 - item name length
//0x0079cb18+3 0x14 - item name length

//0x0079e04e+3 0x1a - price, affects both sides
//0x0079cbd1+2 0x1f - weight, affects both sides
//0x0079ccbc+2 0x21 - [T], affects both sides

//0x0079ef96+2 0x29 - "Value:"
//0x0079d84d+2 0x39 - "Max weight"
//0x0079d779+2 0x2a - "offer marked"
//0x0079d07c+2 0x2a - "view good"

//0x0079c314+1 0x3b - our side name (center)
//0x0079cd6b+7 0x28 - our item name (+2)

//0x002e04cf+2 0x27 - border left
//0x002e0540+2 XXXX - border right

struct traderesize_hook : public df::viewscreen_tradegoodsst
{
    typedef df::viewscreen_tradegoodsst interpose_base;

    DEFINE_VMETHOD_INTERPOSE(void, render, ())
    {
        static bool checked = false, ok = false;

        if (!checked)
        {
            checked = true;

            //check only some of the addresses
            ok =
                *(unsigned char*)(0x002e04cf+2) == 0x27 &&
                *(unsigned char*)(0x0079ef96+2) == 0x29 &&
                *(unsigned char*)(0x0079cbd1+2) == 0x1f &&
                *(unsigned char*)(0x0079cb2a+4) == 0x14;

            if (ok)
            {
                //fixing drawing of the right border
                unsigned char t1[] = { 0x6b, 0xd1, 0x00 }; //imul edx, ecx, XXX
                Core::getInstance().p->patchMemory((void*)(0x002e0540), t1, sizeof(t1));

                unsigned char t2[] = { 0x01, 0xf2, 0x90 }; //add edx, esi; nop
                Core::getInstance().p->patchMemory((void*)(0x002e0545), t2, sizeof(t2));
            }
        }

        if (ok)
        {
            static int lastw = -1;
            if (gps->dimx != lastw)
            {
                lastw = gps->dimx;

                unsigned char x1 = lastw/2-1, x;

                //border
                x = x1;
                Core::getInstance().p->patchMemory((void*)(0x002e04cf+2), &x, 1);
                x = x1 + 1;
                Core::getInstance().p->patchMemory((void*)(0x002e0540+2), &x, 1);

                x = x1 + 1 + 2;
                Core::getInstance().p->patchMemory((void*)(0x0079d07c+2), &x, 1); //view good
                Core::getInstance().p->patchMemory((void*)(0x0079d779+2), &x, 1); //offer marked

                x = x1 + 1 + 1;
                Core::getInstance().p->patchMemory((void*)(0x0079ef96+2), &x, 1); //value

                x = x1 + 1 + 1 + 16;
                Core::getInstance().p->patchMemory((void*)(0x0079d84d+2), &x, 1); //max weight            

                x = x1 + 1 + 2 - 2;
                Core::getInstance().p->patchMemory((void*)(0x0079cd6b+7), &x, 1); //item name

                x = x1 - 2 - 3;
                Core::getInstance().p->patchMemory((void*)(0x0079ccbc+2), &x, 1); //[T]

                x = x1 - 2 - 3;
                Core::getInstance().p->patchMemory((void*)(0x0079cbd1+2), &x, 1); //item weight

                x = x1 - 2 - 3 - 5;
                Core::getInstance().p->patchMemory((void*)(0x0079e04e + 3), &x, 1); //item price

                x = x1 - 2 - 3 - 5 - 5;
                Core::getInstance().p->patchMemory((void*)(0x0079cb2a+4), &x, 1); //item name len
                Core::getInstance().p->patchMemory((void*)(0x0079cb18+3), &x, 1); //item name len

                x = (x1 + 2 + lastw) / 2;
                Core::getInstance().p->patchMemory((void*)(0x0079c314+1), &x, 1); //our side name
            }
        }

        INTERPOSE_NEXT(render)();
    }
};

IMPLEMENT_VMETHOD_INTERPOSE(traderesize_hook, render);
#endif

DFhackCExport command_result plugin_init ( color_ostream &out, vector <PluginCommand> &commands)
{
    auto dflags = init->display.flag;
    if (!dflags.is_set(init_display_flags::USE_GRAPHICS))
    {
        out.color(COLOR_RED);
        out << "TWBT: GRAPHICS is not enabled in init.txt" << std::endl;
        out.color(COLOR_RESET);
        return CR_OK;
    }
    if (dflags.is_set(init_display_flags::RENDER_2D) ||
        dflags.is_set(init_display_flags::ACCUM_BUFFER) ||
        dflags.is_set(init_display_flags::FRAME_BUFFER) ||
        dflags.is_set(init_display_flags::TEXT) ||
        dflags.is_set(init_display_flags::PARTIAL_PRINT))
    {
        out.color(COLOR_RED);
        out << "TWBT: PRINT_MODE must be set to STANDARD or VBO in init.txt" << std::endl;
        out.color(COLOR_RESET);
        return CR_OK;        
    }

    out2 = &out;
    
#ifdef WIN32
    load_multi_pdim = (void (*)(void *tex, const string &filename, long *tex_pos, long dimx,
        long dimy, bool convert_magenta, long *disp_x, long *disp_y)) (0x00a52670+(Core::getInstance().vinfo->getRebaseDelta()));    
#elif defined(__APPLE__)
    load_multi_pdim = (void (*)(void *tex, const string &filename, long *tex_pos, long dimx,
        long dimy, bool convert_magenta, long *disp_x, long *disp_y)) 0x00cfbbb0;    
#else
    load_multi_pdim = (void (*)(void *tex, const string &filename, long *tex_pos, long dimx,
        long dimy, bool convert_magenta, long *disp_x, long *disp_y)) dlsym(RTLD_DEFAULT,"_ZN8textures15load_multi_pdimERKSsPlllbS2_S2_");    
#endif

    bad_item_flags.whole = 0;
    bad_item_flags.bits.in_building = true;
    bad_item_flags.bits.garbage_collect = true;
    bad_item_flags.bits.removed = true;
    bad_item_flags.bits.dead_dwarf = true;
    bad_item_flags.bits.murder = true;
    bad_item_flags.bits.construction = true;
    bad_item_flags.bits.in_inventory = true;
    bad_item_flags.bits.in_chest = true;

    //Main tileset
    struct tileset ts;
    memcpy(ts.small_texpos, df::global::init->font.small_font_texpos, sizeof(ts.small_texpos));
    memcpy(ts.large_texpos, df::global::init->font.large_font_texpos, sizeof(ts.large_texpos));
    tilesets.push_back(ts);

    memset(override_defs, sizeof(struct tileref)*256, 0);

    has_textfont = get_font_paths();
    has_overrides |= load_overrides();
    if (has_textfont || has_overrides)
        hook();
    if (!has_textfont)
    {
        out.color(COLOR_YELLOW);
        out << "TWBT: FONT and GRAPHICS_FONT are the same" << std::endl;
        out.color(COLOR_RESET);        
    }

#ifdef __APPLE__
    INTERPOSE_HOOK(traderesize_hook, render).apply(true);
#endif

    wsthread = new tthread::thread(wsthreadmain, 0);    

    return CR_OK;
}

DFhackCExport command_result plugin_shutdown ( color_ostream &out )
{
    if (enabled)
        unhook();

#ifdef __APPLE__
    INTERPOSE_HOOK(traderesize_hook, render).apply(false);
#endif    

    return CR_OK;
}


static SDL::Key mapInputCodeToSDL( const uint32_t code )
{
#define MAP(a, b) case a: return b;
    switch (code)
    {
    MAP(96, SDL::K_KP0);
    MAP(97, SDL::K_KP1);
    MAP(98, SDL::K_KP2);
    MAP(99, SDL::K_KP3);
    MAP(100, SDL::K_KP4);
    MAP(101, SDL::K_KP5);
    MAP(102, SDL::K_KP6);
    MAP(103, SDL::K_KP7);
    MAP(104, SDL::K_KP8);
    MAP(105, SDL::K_KP9);
    MAP(144, SDL::K_NUMLOCK);

    MAP(111, SDL::K_KP_DIVIDE);
    MAP(106, SDL::K_KP_MULTIPLY);
    MAP(109, SDL::K_KP_MINUS);
    MAP(107, SDL::K_KP_PLUS);

    MAP(33, SDL::K_PAGEUP);
    MAP(34, SDL::K_PAGEDOWN);
    MAP(35, SDL::K_END);
    MAP(36, SDL::K_HOME);
    MAP(46, SDL::K_DELETE);

    MAP(112, SDL::K_F1);
    MAP(113, SDL::K_F2);
    MAP(114, SDL::K_F3);
    MAP(115, SDL::K_F4);
    MAP(116, SDL::K_F5);
    MAP(117, SDL::K_F6);
    MAP(118, SDL::K_F7);
    MAP(119, SDL::K_F8);
    MAP(120, SDL::K_F9);
    MAP(121, SDL::K_F10);
    MAP(122, SDL::K_F11);
    MAP(123, SDL::K_F12);

    MAP(37, SDL::K_LEFT);
    MAP(39, SDL::K_RIGHT);
    MAP(38, SDL::K_UP);
    MAP(40, SDL::K_DOWN);

    MAP(188, SDL::K_LESS);
    MAP(190, SDL::K_GREATER);

    MAP(13, SDL::K_RETURN);

    //MAP(16, SDL::K_LSHIFT);
    //MAP(17, SDL::K_LCTRL);
    //MAP(18, SDL::K_LALT);

    MAP(27, SDL::K_ESCAPE);
#undef MAP
    }
    if (code <= 177)
        return (SDL::Key)code;
    return SDL::K_UNKNOWN;
}

void simkey(int down, int mod, SDL::Key sym, int unicode)
{
    SDL::Event event;
    memset(&event, 0, sizeof(event));

    event.type = down ? SDL::ET_KEYDOWN : SDL::ET_KEYUP;
    event.key.state = down ? SDL::BTN_PRESSED : SDL::BTN_RELEASED;
    event.key.ksym.mod = (SDL::Mod)mod;
    event.key.ksym.sym = sym;
    event.key.ksym.unicode = unicode;

    SDL_PushEvent(&event);    
}

void setactive(int newidx)
{
    if (newidx >= clients.size())
        newidx = clients.size() > 0 ? 0 : -1;

    activeidx = newidx;
    if (activeidx == -1)
        return;

    Client *newcl = clients[activeidx];
    newcl->active = true;
    newcl->atime = newcl->itime = time(NULL);
    memset(newcl->mod, 0, sizeof(newcl->mod));

    if (!(*df::global::pause_state))
    {
        simkey(1, 0, SDL::K_SPACE, ' ');
        simkey(0, 0, SDL::K_SPACE, ' ');
    }
    //shownextturn = true;

    *out2 << "active " << activeidx << " " << (activeidx == -1 ? "-" : nopoll_conn_host(clients[activeidx]->conn)) << std::endl;    
}

void listener_on_message (noPollCtx * ctx, noPollConn * conn, noPollMsg * msg, noPollPtr user_data)
{
    Client *cl = (Client*) user_data;
    int idx = 0;
    for (int i = 0; i < clients.size(); i++)
    {
        if (clients[i] == cl)
        {
            idx = i;
            break;
        }
    }    

    const unsigned char *mdata = (const unsigned char*) nopoll_msg_get_payload(msg);
    int msz = nopoll_msg_get_payload_size(msg);

    if (mdata[0] == 112 && msz == 3)
    {
        if (cl->active)
        {
            newwidth = mdata[1];
            newheight = mdata[2];
            needsresize = true;
        }
    }
    else if (mdata[0] == 111 && msz == 4)
    {
        if (cl->active)
        {
            cl->itime = time(NULL);

            SDL::Key k = mdata[2] ? (SDL::Key)mdata[2] : mapInputCodeToSDL(mdata[1]);
            if (k != SDL::K_UNKNOWN)
            {
                int jsmods = mdata[3];
                int sdlmods = 0;

                if (jsmods & 1)
                {
                    simkey(1, 0, SDL::K_LALT, 0);
                    sdlmods |= SDL::KMOD_ALT;
                }
                if (jsmods & 2)
                {
                    simkey(1, 0, SDL::K_LSHIFT, 0);
                    sdlmods |= SDL::KMOD_SHIFT;
                }
                if (jsmods & 4)
                {
                    simkey(1, 0, SDL::K_LCTRL, 0);
                    sdlmods |= SDL::KMOD_CTRL;
                }

                simkey(1, sdlmods, k, mdata[2]);
                simkey(0, sdlmods, k, mdata[2]);

                if (jsmods & 1)
                    simkey(0, 0, SDL::K_LALT, 0);
                if (jsmods & 2)
                    simkey(0, 0, SDL::K_LSHIFT, 0);
                if (jsmods & 4)
                    simkey(0, 0, SDL::K_LCTRL, 0);
            }
        }
    }
    /*else if (mdata[0] == 113)
    {
        int x = (((unsigned int)mdata[1]<<8) | mdata[2]);
        int y = (((unsigned int)mdata[3]<<8) | mdata[4]);
        SDL::Event event;
            memset( &event, 0, sizeof(event) );
            event.type = 4;
            event.motion.type = 4;
            event.motion.state = 0;
            event.motion.x = x;
            event.motion.y = y;
        SDL_PushEvent( &event );

        gps->mouse_x = x;
        gps->mouse_y = y;
    }
    else if (mdata[0] == 114)
    {
        SDL::Event event;

        memset( &event, 0, sizeof(event) );
        event.type = SDL::ET_MOUSEBUTTONDOWN;
        event.button.type = 5;//SDL::SDL_MOUSEBUTTONDOWN;
        event.button.which = 0;
        event.button.button = 1;
        event.button.state = SDL::BTN_PRESSED;
        event.button.x = 100;
        event.button.y = 100;
        SDL_PushEvent( &event );

        memset( &event, 0, sizeof(event) );
        event.type = SDL::ET_MOUSEBUTTONUP;
        event.button.type = 5;//SDL::SDL_MOUSEBUTTONUP;
        event.button.which = 0;
        event.button.button = 1;
        event.button.state = SDL::BTN_RELEASED;
        event.button.x = 100;
        event.button.y = 100;
        SDL_PushEvent( &event );

        //nopoll_conn_send_binary (conn, "\0\0\0", 3);
    }*/
    else if (mdata[0] == 115)
    {
        memset(cl->mod, 0, sizeof(cl->mod));
    }
    else
    {
        bool soon = false;
        if (activeidx != -1 && clients.size() > 1)
        {
            time_t now = time(NULL);
            int played = now - clients[activeidx]->atime;
            int idle = now - clients[activeidx]->itime;
            if (played >= PLAYTIME || idle >= IDLETIME)
                setactive(activeidx+1);
            else if (cl->active)
            {
                if (PLAYTIME - played < 60)
                    soon = true;
            }
        }
        int sent = 1;

        unsigned char *b = buf;
        *(b++) = 110;

        int qpos = idx - activeidx;
        if (qpos < 0)
            qpos = -qpos;
        if (soon)
            qpos |= 128;
        *(b++) = qpos;

        *(b++) = gps->dimx;
        *(b++) = gps->dimy;

        unsigned char *emptyb = b;
        unsigned char *mod = cl->mod;

        do
        {
        //int tile = 0;
        for (int y = 0; y < gps->dimy; y++)
        {
            for (int x = 0; x < gps->dimx; x++)
            {
                const int tile = x * gps->dimy + y;
                unsigned char *s = sc + tile*4;
                if (mod[tile])
                    continue;

                *(b++) = x;
                *(b++) = y;
                *(b++) = s[0];
                *(b++) = s[2];

                int bold = (s[3] != 0) * 8;
                int fg   = (s[1] + bold) % 16;

                *(b++) = fg;
                mod[tile] = 1;
            }
        }
    
        if (b == emptyb)
        {
            nopoll_conn_send_binary (conn, "\0", 1);
            //tthread::this_thread::sleep_for(tthread::chrono::milliseconds(1000/60));
        }
        else
        {
            sent = 1;
            nopoll_conn_send_binary (conn, (const char*)buf, (int)(b-buf));
        }
        } while (!sent);
    }

    nopoll_msg_unref(msg);
    return;
}

void listener_on_close (noPollCtx * ctx, noPollConn * conn, noPollPtr user_data)
{
    Client *cl = (Client*) user_data;
    for (int i = 0; i < clients.size(); i++)
    {
        if (clients[i] == cl)
        {
            clients.erase(clients.begin()+i);
            delete cl;

            if (activeidx == i)
                setactive(activeidx);

            break;
        }
    }

    *out2 << "disconnected " << nopoll_conn_host(conn) << " count " << clients.size() << " active " << activeidx << " " << (activeidx == -1 ? "-" : nopoll_conn_host(clients[activeidx]->conn)) << std::endl;    
}

nopoll_bool listener_on_accept (noPollCtx * ctx, noPollConn * conn, noPollPtr user_data)
{
    if (clients.size() >= 100)
        return false;

    Client *cl = new Client;
    cl->conn = conn;
    cl->active = false;

    nopoll_conn_set_on_msg(conn, listener_on_message, cl);
    nopoll_conn_set_on_close(conn, listener_on_close, cl);

    clients.push_back(cl);

    if (activeidx == -1)
        setactive(clients.size() - 1);

    *out2 << "connected " << nopoll_conn_host(conn) << " count " << clients.size() << " active " << activeidx << " " << nopoll_conn_host(clients[activeidx]->conn) << std::endl;    

    return true;
}

void wsthreadmain(void *dummy)
{
    noPollCtx *ctx = nopoll_ctx_new ();
    if (!ctx)
    {
        // error some handling code here
    }

    // create a listener to receive connections on port 1234
    noPollConn *listener = nopoll_listener_new (ctx, "0.0.0.0", "1234");
    if (!nopoll_conn_is_ok(listener)) {
         // some error handling here
    }
 
    // now set a handler that will be called when a message (fragment or not) is received
    //nopoll_ctx_set_on_msg (ctx, listener_on_message, NULL);
    nopoll_ctx_set_on_accept (ctx, listener_on_accept, NULL);
 
    // now call to wait for the loop to notify events 
    nopoll_loop_wait (ctx, 0);    
}