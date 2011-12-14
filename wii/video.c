/*  SSNES - A Super Nintendo Entertainment System (SNES) Emulator frontend for libsnes.
 *  Copyright (C) 2010-2011 - Hans-Kristian Arntzen
 *
 *  Some code herein may be based on code found in BSNES.
 * 
 *  SSNES is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  SSNES is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with SSNES.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "../driver.h"
#include "../general.h"
#include <gccore.h>
#include <ogcsys.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

// All very hardcoded for now.

static void *g_framebuf[2];
static unsigned g_framebuf_index;

struct
{
   uint16_t data[512 * 512];
   GXTexObj obj;
} static g_tex ATTRIBUTE_ALIGN(32);

static uint8_t gx_fifo[256 * 1024] ATTRIBUTE_ALIGN(32);
static uint8_t display_list[256] ATTRIBUTE_ALIGN(32);
static size_t display_list_size;

static void setup_video_mode(GXRModeObj *mode, void *framebuf[2])
{
   VIDEO_Configure(mode);
   VIDEO_Flush();
   for (unsigned i = 0; i < 2; i++)
      VIDEO_ClearFrameBuffer(mode, framebuf[i], COLOR_BLACK);
   VIDEO_SetNextFramebuffer(framebuf[0]);
   VIDEO_SetBlack(false);
   VIDEO_Flush();
   VIDEO_WaitVSync();
}

static float verts[] = {
   -1, -1, 0.5,
   -1,  1, 0.5,
    1, -1, 0.5,
    1,  1, 0.5,
};

static float tex_coords[] = {
   0, 0,
   0, 1,
   1, 0,
   1, 1,
};

static void init_vtx(void)
{
   GX_SetViewport(0, 0, 640, 480, 0, 1);
   GX_SetDispCopyYScale(1.0f);
   GX_SetScissor(0, 0, 640, 480);
   GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
   GX_SetZMode(GX_DISABLE, GX_NEVER, GX_DISABLE);
   GX_SetColorUpdate(GX_TRUE);

   Mtx m;
   guMtxIdentity(m);
   GX_LoadProjectionMtx(m, GX_ORTHOGRAPHIC);
   GX_LoadPosMtxImm(m, GX_PNMTX0);

   GX_ClearVtxDesc();
   GX_SetVtxDesc(GX_VA_POS, GX_INDEX8);
   GX_SetVtxDesc(GX_VA_TEX0, GX_INDEX8);

   GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
   GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
   GX_SetArray(GX_VA_POS, verts, 3 * sizeof(float));
   GX_SetArray(GX_VA_TEX0, tex_coords, 2 * sizeof(float));

   GX_SetNumTexGens(1);
   GX_SetNumChans(0);
   GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
   GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);

   GX_InvVtxCache();
}

static void init_texture(void)
{
   GX_InitTexObj(&g_tex.obj, g_tex.data, 512, 512, GX_TF_RGB5A3, GX_MIRROR, GX_MIRROR, GX_FALSE);
   GX_InitTexObjLOD(&g_tex.obj, GX_LINEAR, GX_LINEAR, 0, 10, 0, GX_ENABLE, GX_FALSE, GX_ANISO_1);
   GX_LoadTexObj(&g_tex.obj, GX_TEXMAP0);

   Mtx m;
   guMtxIdentity(m);
   GX_LoadTexMtxImm(m, GX_TEXMTX0, GX_MTX2x4);
   GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_TEXMTX0);

   GX_InvalidateTexAll();
}

static void build_disp_list(void)
{
   DCInvalidateRange(display_list, sizeof(display_list));
   GX_BeginDispList(display_list, sizeof(display_list));
   GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT0, 4);
   for (unsigned i = 0; i < 4; i++)
   {
      GX_Position1x8(i);
      GX_TexCoord1x8(i);
   }
   GX_End();
   display_list_size = GX_EndDispList();
}

static void *wii_init(const video_info_t *video,
      const input_driver_t **input, void **input_data)
{
   static bool inited = false;
   if (!inited)
   {
      VIDEO_Init();
      inited = true;
   }

   for (unsigned i = 0; i < 2; i++)
   {
      g_framebuf[i] = memalign(32, 640 * 528 * 6);
      DCInvalidateRange(g_framebuf[i], 640 * 528 * 6);
      g_framebuf[i] = MEM_K0_TO_K1(g_framebuf[i]);
   }

   setup_video_mode(VIDEO_GetPreferredMode(NULL), g_framebuf);

   GX_Init(gx_fifo, sizeof(gx_fifo));
   GX_SetDispCopyGamma(GX_GM_1_0);
   GX_SetCullMode(GX_CULL_NONE);

   init_vtx();
   init_texture();
   build_disp_list();

   *input = NULL;
   *input_data = NULL;
   return (void*)-1;
}

static void update_texture(const uint16_t *src,
      unsigned width, unsigned height, unsigned pitch)
{
   float tex_w = (float)width / 512;
   float tex_h = (float)height / 512;

   tex_coords[4] = tex_coords[6] = tex_w;
   tex_coords[3] = tex_coords[7] = tex_h;

   uint16_t *dst = g_tex.data;
   for (unsigned i = 0; i < height; i++,
         dst += 512, src += pitch >> 1)
   {
      memcpy(dst, src, width * sizeof(uint16_t));
   }
   
   GX_InvalidateTexAll();
}

static bool wii_frame(void *data, const void *frame,
      unsigned width, unsigned height, unsigned pitch,
      const char *msg)
{
   (void)data;
   (void)msg;

   GX_SetCopyClear((GXColor) { 0, 0, 0, 0xff }, GX_MAX_Z24);

   update_texture(frame, width, height, pitch);
   GX_CallDispList(display_list, display_list_size);
   GX_DrawDone();

   g_framebuf_index ^= 1;
   GX_CopyDisp(g_framebuf[g_framebuf_index], GX_TRUE);
   VIDEO_SetNextFramebuffer(g_framebuf[g_framebuf_index]);
   VIDEO_Flush();
   VIDEO_WaitVSync();

   return true;
}

static void wii_set_nonblock_state(void *data, bool state)
{
   (void)data;
   (void)state;
}

static bool wii_alive(void *data)
{
   (void)data;
   return true;
}

static bool wii_focus(void *data)
{
   (void)data;
   return true;
}

static void wii_free(void *data)
{
   (void)data;
   GX_AbortFrame();
   GX_Flush();
   VIDEO_SetBlack(true);
   VIDEO_Flush();

   for (unsigned i = 0; i < 2; i++)
      free(MEM_K1_TO_K0(g_framebuf[i]));
}

const video_driver_t video_wii = {
   .init = wii_init,
   .frame = wii_frame,
   .alive = wii_alive,
   .set_nonblock_state = wii_set_nonblock_state,
   .focus = wii_focus,
   .free = wii_free,
   .ident = "wii"
};

