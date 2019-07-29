/* *  This file is part of Permafrost Engine. 
 *  Copyright (C) 2019 Eduard Permyakov 
 *
 *  Permafrost Engine is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Permafrost Engine is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * 
 *  Linking this software statically or dynamically with other modules is making 
 *  a combined work based on this software. Thus, the terms and conditions of 
 *  the GNU General Public License cover the whole combination. 
 *  
 *  As a special exception, the copyright holders of Permafrost Engine give 
 *  you permission to link Permafrost Engine with independent modules to produce 
 *  an executable, regardless of the license terms of these independent 
 *  modules, and to copy and distribute the resulting executable under 
 *  terms of your choice, provided that you also meet, for each linked 
 *  independent module, the terms and conditions of the license of that 
 *  module. An independent module is a module which is not derived from 
 *  or based on Permafrost Engine. If you modify Permafrost Engine, you may 
 *  extend this exception to your version of Permafrost Engine, but you are not 
 *  obliged to do so. If you do not wish to do so, delete this exception 
 *  statement from your version.
 *
 */

#include "render_gl.h"
#include "mesh.h"
#include "texture.h"
#include "vertex.h"
#include "shader.h"
#include "gl_assert.h"
#include "gl_uniforms.h"
#include "public/render.h"
#include "../camera.h"
#include "../config.h"
#include "../main.h"
#include "../map/public/tile.h"
#include "../map/public/map.h"
#include "../game/public/game.h"

#include <assert.h>
#include <string.h>


struct render_water_ctx{
    struct mesh    surface;
    struct texture dudv;
    struct texture normal;
};

struct water_gl_state{
    GLint    viewport[4];
    GLint    fb;
};

#define WATER_LVL       (-1.0f * Y_COORDS_PER_TILE + 2.0f)
#define ARR_SIZE(a)     (sizeof(a)/sizeof(a[0])) 
#define DUDV_PATH       "assets/water_textures/dudvmap.png"
#define NORM_PATH       "assets/water_textures/normalmap.png"
#define WBUFF_RES_X     800

/*****************************************************************************/
/* STATIC VARIABLES                                                          */
/*****************************************************************************/

static struct render_water_ctx s_ctx;

/*****************************************************************************/
/* STATIC FUNCTIONS                                                          */
/*****************************************************************************/

static void save_gl_state(struct water_gl_state *out)
{
    glGetIntegerv(GL_VIEWPORT, out->viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &out->fb);
}

static void restore_gl_state(const struct water_gl_state *in)
{
    glBindFramebuffer(GL_FRAMEBUFFER, in->fb);
    glViewport(in->viewport[0], in->viewport[1], in->viewport[2], in->viewport[3]);

    /* Restore the view matrix and camera position uniforms as 
     * they have been clobbered by the reflection texture rendering */
    DECL_CAMERA_STACK(cam);
    memset(cam, 0, sizeof(cam));
    Camera_SetPos((struct camera*)cam, G_ActiveCamPos());
    Camera_SetDir((struct camera*)cam, G_ActiveCamDir());
    Camera_TickFinishPerspective((struct camera*)cam);
}

static int height_for_width(int width)
{
    int viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    float ar = (float)viewport[2] / viewport[3];
    return width / ar;
}

static GLuint make_new_tex(int width, int height)
{
    GLuint ret;
    glGenTextures(1, &ret);
    glBindTexture(GL_TEXTURE_2D, ret);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_BYTE, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    GL_ASSERT_OK();
    return ret;
}

static void render_refraction_tex(GLuint tex)
{
    GLint texw, texh;
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &texw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &texh);

    /* Create framebuffer object */
    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);

    GLuint depth_rb;
    glGenRenderbuffers(1, &depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, texw, texh);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0);

    GLenum draw_buffs[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(ARR_SIZE(draw_buffs), draw_buffs);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    /* Clip everything above the water surface */
    glEnable(GL_CLIP_DISTANCE0);
    vec4_t plane_eq = (vec4_t){0.0f, -1.0f, 0.0f, WATER_LVL};
    R_GL_SetClipPlane(plane_eq);

    /* Render to the texture */
    glViewport(0, 0, texw, texh);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    G_RenderMapAndEntities();

    /* Clean up framebuffer */
    glDeleteRenderbuffers(1, &depth_rb);
    glDeleteFramebuffers(1, &fb);
    glDisable(GL_CLIP_DISTANCE0);

    GL_ASSERT_OK();
}

static void render_reflection_tex(GLuint tex)
{
    GLint texw, texh;
    glBindTexture(GL_TEXTURE_2D, tex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &texw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &texh);

    DECL_CAMERA_STACK(cam);
    memset(cam, 0, sizeof(cam));
    vec3_t cam_pos = G_ActiveCamPos();
    vec3_t cam_dir = G_ActiveCamDir();
    cam_pos.y -= (cam_pos.y - WATER_LVL) * 2.0f;
    cam_dir.y *= -1.0f;
    Camera_SetPos((struct camera*)cam, cam_pos);
    Camera_SetDir((struct camera*)cam, cam_dir);
    Camera_TickFinishPerspective((struct camera*)cam);

    /* Create framebuffer object */
    GLuint fb;
    glGenFramebuffers(1, &fb);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);

    GLuint depth_rb;
    glGenRenderbuffers(1, &depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT, texw, texh);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0);

    GLenum draw_buffs[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(ARR_SIZE(draw_buffs), draw_buffs);
    assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    /* Clip everything below the water surface */
    glEnable(GL_CLIP_DISTANCE0);
    vec4_t plane_eq = (vec4_t){0.0f, 1.0f, 0.0f, WATER_LVL};
    R_GL_SetClipPlane(plane_eq);

    /* Render to the texture */
    glViewport(0, 0, texw, texh);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    G_RenderMapAndEntities();

    /* Clean up framebuffer */
    glDeleteRenderbuffers(1, &depth_rb);
    glDeleteFramebuffers(1, &fb);
    glDisable(GL_CLIP_DISTANCE0);

    GL_ASSERT_OK();
}

/*****************************************************************************/
/* EXTERN FUNCTIONS                                                          */
/*****************************************************************************/

bool R_GL_WaterInit(void)
{
    bool ret = true;

    ret = R_Texture_Load(g_basepath, DUDV_PATH, &s_ctx.dudv.id);
    if(!ret)
        goto fail_dudv;
    s_ctx.dudv.tunit = GL_TEXTURE0;
    
    ret = R_Texture_Load(g_basepath, NORM_PATH, &s_ctx.normal.id);
    if(!ret)
        goto fail_normal;
    s_ctx.normal.tunit = GL_TEXTURE1;

    const vec3_t tl = (vec3_t){+1.0f, WATER_LVL, +1.0f};
    const vec3_t tr = (vec3_t){-1.0f, WATER_LVL, +1.0f};
    const vec3_t bl = (vec3_t){+1.0f, WATER_LVL, -1.0f};
    const vec3_t br = (vec3_t){-1.0f, WATER_LVL, -1.0f};

    vec3_t vbuff[] = {
        tl, bl, tr,
        bl, br, tr
    };

    glGenVertexArrays(1, &s_ctx.surface.VAO);
    glBindVertexArray(s_ctx.surface.VAO);

    glGenBuffers(1, &s_ctx.surface.VBO);
    glBindBuffer(GL_ARRAY_BUFFER, s_ctx.surface.VBO);
    glBufferData(GL_ARRAY_BUFFER, ARR_SIZE(vbuff) * sizeof(struct vertex), vbuff, GL_STATIC_DRAW);
    s_ctx.surface.num_verts = ARR_SIZE(vbuff);

    /* Attribute 0 - position */
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vec3_t), (void*)0);
    glEnableVertexAttribArray(0);

    GL_ASSERT_OK();
    return ret;

fail_normal:
    R_Texture_Free(DUDV_PATH);
fail_dudv:
    return ret;
}

void R_GL_WaterShutdown(void)
{
    assert(s_ctx.dudv.id > 0);
    assert(s_ctx.normal.id > 0);
    assert(s_ctx.surface.VBO > 0);
    assert(s_ctx.surface.VAO > 0);

    R_Texture_Free(DUDV_PATH);
    R_Texture_Free(NORM_PATH);

    glDeleteBuffers(1, &s_ctx.surface.VAO);
    glDeleteBuffers(1, &s_ctx.surface.VBO);
    memset(&s_ctx, 0, sizeof(s_ctx));
}

void R_GL_DrawWater(const struct map *map)
{
    struct water_gl_state state;
    save_gl_state(&state);

    GLuint refract_tex = make_new_tex(WBUFF_RES_X, height_for_width(WBUFF_RES_X));
    assert(refract_tex > 0);
    render_refraction_tex(refract_tex);

    GLuint reflect_tex = make_new_tex(WBUFF_RES_X, height_for_width(WBUFF_RES_X));
    assert(reflect_tex > 0);
    render_reflection_tex(reflect_tex);

    restore_gl_state(&state);

    GLuint shader_prog = R_Shader_GetProgForName("mesh.static.colored");
    glUseProgram(shader_prog);

    vec4_t blue = (vec4_t){0.0f, 0.0f, 1.0f, 1.0f};

    GLuint loc = glGetUniformLocation(shader_prog, GL_U_COLOR);
    glUniform4fv(loc, 1, blue.raw);

    R_Texture_GL_Activate(&s_ctx.dudv, shader_prog);
    R_Texture_GL_Activate(&s_ctx.normal, shader_prog);

    vec3_t pos = M_GetCenterPos(map);
    mat4x4_t trans;
    PFM_Mat4x4_MakeTrans(pos.x, pos.y, pos.z, &trans);

    struct map_resolution res;
    M_GetResolution(map, &res);
    float half_x = (res.chunk_w * res.tile_w * X_COORDS_PER_TILE) / 2.0f;
    float half_z = (res.chunk_h * res.tile_h * Z_COORDS_PER_TILE) / 2.0f;

    mat4x4_t scale;
    PFM_Mat4x4_MakeScale(half_x, 1.0f, half_z, &scale);

    mat4x4_t model;
    PFM_Mat4x4_Mult4x4(&trans, &scale, &model);

    loc = glGetUniformLocation(shader_prog, GL_U_MODEL);
    glUniformMatrix4fv(loc, 1, GL_FALSE, model.raw);

    glBindVertexArray(s_ctx.surface.VAO);
    glDrawArrays(GL_TRIANGLES, 0, s_ctx.surface.num_verts);

cleanup:
    glDeleteTextures(1, &refract_tex);
    glDeleteTextures(1, &reflect_tex);

    GL_ASSERT_OK();
}

