/*
 * vigs
 *
 * Copyright (c) 2000 - 2013 Samsung Electronics Co., Ltd. All rights reserved.
 *
 * Contact:
 * Stanislav Vorobiov <s.vorobiov@samsung.com>
 * Jinhyung Jo <jinhyung.jo@samsung.com>
 * YeongKyoon Lee <yeongkyoon.lee@samsung.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Contributors:
 * - S-Core Co., Ltd
 *
 */

#include "vigs_gl_backend.h"
#include "vigs_log.h"
#include <windows.h>
#include <wingdi.h>
#include <GL/gl.h>
#include <GL/wglext.h>

HWND vigs_window = 0;

#define VIGS_WGL_WIN_CLASS "VIGSWinClass"

#define VIGS_WGL_GET_PROC(proc_type, proc_name, label) \
    do { \
        gl_backend_wgl->proc_name = \
            (proc_type)GetProcAddress(gl_backend_wgl->handle, #proc_name); \
        if (!gl_backend_wgl->proc_name) { \
            VIGS_LOG_CRITICAL("Unable to load " #proc_name " symbol"); \
            goto label; \
        } \
    } while (0)

#define VIGS_WGL_GET_EXT_PROC(ext_name, proc_type, proc_name) \
    do { \
        if ((strstr(ext_str, #ext_name " ") == NULL)) { \
            VIGS_LOG_CRITICAL("%s extension not supported", #ext_name); \
            goto fail; \
        } \
        gl_backend_wgl->proc_name = \
            (proc_type)gl_backend_wgl->wglGetProcAddress((LPCSTR)#proc_name); \
        if (!gl_backend_wgl->proc_name) { \
            VIGS_LOG_CRITICAL("Unable to load " #proc_name " symbol"); \
            goto fail; \
        } \
    } while (0)

#define VIGS_GL_GET_PROC(func, proc_name) \
    do { \
        *(void**)(&gl_backend_wgl->base.func) = gl_backend_wgl->wglGetProcAddress((LPCSTR)#proc_name); \
        if (!gl_backend_wgl->base.func) { \
            *(void**)(&gl_backend_wgl->base.func) = GetProcAddress(gl_backend_wgl->handle, #proc_name); \
            if (!gl_backend_wgl->base.func) { \
                VIGS_LOG_CRITICAL("Unable to load " #proc_name " symbol"); \
                goto fail; \
            } \
        } \
    } while (0)

#define VIGS_GL_GET_PROC_OPTIONAL(func, proc_name) \
    do { \
        *(void**)(&gl_backend_wgl->base.func) = gl_backend_wgl->wglGetProcAddress((LPCSTR)#proc_name); \
        if (!gl_backend_wgl->base.func) { \
            *(void**)(&gl_backend_wgl->base.func) = GetProcAddress(gl_backend_wgl->handle, #proc_name); \
        } \
    } while (0)

typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTPROC)(HDC hdl);
typedef BOOL (WINAPI *PFNWGLDELETECONTEXTPROC)(HGLRC hdl);
typedef PROC (WINAPI *PFNWGLGETPROCADDRESSPROC)(LPCSTR sym);
typedef BOOL (WINAPI *PFNWGLMAKECURRENTPROC)(HDC dev_ctx, HGLRC rend_ctx);
typedef BOOL (WINAPI *PFNWGLSHARELISTSPROC)(HGLRC ctx1, HGLRC ctx2);
typedef HGLRC (WINAPI *PFNWGLGETCURRENTCONTEXTPROC)(void);

struct vigs_gl_backend_wgl
{
    struct vigs_gl_backend base;

    HINSTANCE handle;

    PFNWGLCREATECONTEXTPROC wglCreateContext;
    PFNWGLDELETECONTEXTPROC wglDeleteContext;
    PFNWGLGETPROCADDRESSPROC wglGetProcAddress;
    PFNWGLMAKECURRENTPROC wglMakeCurrent;
    PFNWGLGETCURRENTCONTEXTPROC wglGetCurrentContext;
    PFNWGLSHARELISTSPROC wglShareLists;

    /* WGL extensions */
    PFNWGLGETEXTENSIONSSTRINGEXTPROC wglGetExtensionsStringEXT;
    PFNWGLGETEXTENSIONSSTRINGARBPROC wglGetExtensionsStringARB;
    PFNWGLCHOOSEPIXELFORMATARBPROC wglChoosePixelFormatARB;
    PFNWGLCREATEPBUFFERARBPROC wglCreatePbufferARB;
    PFNWGLGETPBUFFERDCARBPROC wglGetPbufferDCARB;
    PFNWGLRELEASEPBUFFERDCARBPROC wglReleasePbufferDCARB;
    PFNWGLDESTROYPBUFFERARBPROC wglDestroyPbufferARB;

    /* WGL_ARB_create_context */
    PFNWGLCREATECONTEXTATTRIBSARBPROC wglCreateContextAttribsARB;

    HWND win;
    HDC dc;
    HPBUFFERARB sfc;
    HDC sfc_dc;
    HGLRC ctx;
    HPBUFFERARB read_pixels_sfc;
    HDC read_pixels_sfc_dc;
    HGLRC read_pixels_ctx;
};

static bool vigs_gl_backend_wgl_check_gl_version(struct vigs_gl_backend_wgl *gl_backend_wgl,
                                                 bool *is_gl_2)
{
    int config_attribs[] = {
        WGL_SUPPORT_OPENGL_ARB, TRUE,
        WGL_DOUBLE_BUFFER_ARB, TRUE,
        WGL_DRAW_TO_PBUFFER_ARB, TRUE,
        WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB,
        WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
        WGL_RED_BITS_ARB, 8,
        WGL_GREEN_BITS_ARB, 8,
        WGL_BLUE_BITS_ARB, 8,
        WGL_ALPHA_BITS_ARB, 8,
        WGL_COLOR_BITS_ARB, 32,
        WGL_DEPTH_BITS_ARB, 24,
        WGL_STENCIL_BITS_ARB, 8,
        0,
    };
    int ctx_attribs[] =
    {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 1,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };
    bool res = false;
    const char *tmp;
    HWND win;
    HDC dc;
    int config_id = 0;
    UINT n = 0;
    PIXELFORMATDESCRIPTOR pix_fmt;
    HGLRC ctx;

    tmp = getenv("GL_VERSION");

    if (tmp) {
        if (strcmp(tmp, "2") == 0) {
            VIGS_LOG_INFO("GL_VERSION forces OpenGL version to 2.1");
            *is_gl_2 = true;
            res = true;
        } else if (strcmp(tmp, "3_1") == 0) {
            VIGS_LOG_INFO("GL_VERSION forces OpenGL version to 3.1");
            *is_gl_2 = false;
            res = true;
        } else if (strcmp(tmp, "3_1_es3") == 0) {
            VIGS_LOG_INFO("GL_VERSION forces OpenGL version to 3.1 ES3");
            *is_gl_2 = false;
            res = true;
        } else if (strcmp(tmp, "3_2") == 0) {
            VIGS_LOG_INFO("GL_VERSION forces OpenGL version to 3.2");
            *is_gl_2 = false;
            res = true;
        } else {
            VIGS_LOG_CRITICAL("Bad GL_VERSION value = %s", tmp);
        }

        goto out1;
    }

    win = CreateWindow(VIGS_WGL_WIN_CLASS, "VIGSWin",
                       WS_DISABLED | WS_POPUP,
                       0, 0, 1, 1, NULL, NULL, 0, 0);

    if (!win) {
        VIGS_LOG_ERROR("CreateWindow failed");
        goto out1;
    }

    dc = GetDC(win);
    if (!dc) {
        VIGS_LOG_ERROR("GetDC failed");
        goto out2;
    }

    if (!gl_backend_wgl->wglChoosePixelFormatARB(dc,
                                                 config_attribs,
                                                 NULL,
                                                 1,
                                                 &config_id,
                                                 &n) || (n == 0)) {
        VIGS_LOG_ERROR("wglChoosePixelFormatARB failed");
        goto out3;
    }

    if (!DescribePixelFormat(dc,
                             config_id,
                             sizeof(PIXELFORMATDESCRIPTOR),
                             &pix_fmt)) {
        VIGS_LOG_ERROR("DescribePixelFormat failed");
        goto out3;
    }

    if (!SetPixelFormat(dc,
                        config_id,
                        &pix_fmt)) {
        VIGS_LOG_ERROR("SetPixelFormat failed");
        goto out3;
    }

    ctx = gl_backend_wgl->wglCreateContextAttribsARB(dc,
                                                     NULL,
                                                     ctx_attribs);

    *is_gl_2 = (ctx == NULL);
    res = true;

    if (ctx) {
        VIGS_LOG_INFO("Using OpenGL 3.1+ core");
    } else {
        VIGS_LOG_INFO("wglCreateContextAttribsARB failed, using OpenGL 2.1");
    }

    gl_backend_wgl->wglDeleteContext(ctx);

out3:
    ReleaseDC(win, dc);
out2:
    DestroyWindow(win);
out1:

    return res;
}

static int vigs_gl_backend_wgl_choose_config(struct vigs_gl_backend_wgl *gl_backend_wgl)
{
    const int config_attribs[] = {
        WGL_SUPPORT_OPENGL_ARB, TRUE,
        WGL_DOUBLE_BUFFER_ARB, TRUE,
        WGL_DRAW_TO_PBUFFER_ARB, TRUE,
        WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB,
        WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
        WGL_RED_BITS_ARB, 8,
        WGL_GREEN_BITS_ARB, 8,
        WGL_BLUE_BITS_ARB, 8,
        WGL_ALPHA_BITS_ARB, 8,
        WGL_COLOR_BITS_ARB, 32,
        WGL_DEPTH_BITS_ARB, 24,
        WGL_STENCIL_BITS_ARB, 8,
        0,
    };
    int config_id = 0;
    UINT n = 0;
    PIXELFORMATDESCRIPTOR pix_fmt;

    if (!gl_backend_wgl->wglChoosePixelFormatARB(gl_backend_wgl->dc,
                                                 config_attribs,
                                                 NULL,
                                                 1,
                                                 &config_id,
                                                 &n) || (n == 0)) {
        return 0;
    }

    if (!DescribePixelFormat(gl_backend_wgl->dc,
                             config_id,
                             sizeof(PIXELFORMATDESCRIPTOR),
                             &pix_fmt)) {
        return 0;
    }

    if (!SetPixelFormat(gl_backend_wgl->dc,
                        config_id,
                        &pix_fmt)) {
        return 0;
    }

    return config_id;
}

static bool vigs_gl_backend_wgl_create_surface(struct vigs_gl_backend_wgl *gl_backend_wgl,
                                               int config_id,
                                               HPBUFFERARB *sfc,
                                               HDC *sfc_dc)
{
    int surface_attribs[] = {
        WGL_PBUFFER_LARGEST_ARB, FALSE,
        WGL_TEXTURE_TARGET_ARB, WGL_NO_TEXTURE_ARB,
        WGL_TEXTURE_FORMAT_ARB, WGL_NO_TEXTURE_ARB,
        0
    };

    *sfc = gl_backend_wgl->wglCreatePbufferARB(gl_backend_wgl->dc, config_id,
                                               1, 1, surface_attribs);

    if (!*sfc) {
        VIGS_LOG_CRITICAL("wglCreatePbufferARB failed");
        return false;
    }

    *sfc_dc = gl_backend_wgl->wglGetPbufferDCARB(*sfc);

    if (!*sfc_dc) {
        VIGS_LOG_CRITICAL("wglGetPbufferDCARB failed");
        return false;
    }

    return true;
}

static bool vigs_gl_backend_wgl_create_context(struct vigs_gl_backend_wgl *gl_backend_wgl,
                                               HGLRC share_ctx,
                                               HGLRC *ctx)
{
    int attribs[] =
    {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 1,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
    };

    if (gl_backend_wgl->base.is_gl_2) {
        *ctx = gl_backend_wgl->wglCreateContext(gl_backend_wgl->dc);
    } else {
        *ctx = gl_backend_wgl->wglCreateContextAttribsARB(gl_backend_wgl->dc,
                                                          share_ctx,
                                                          attribs);
    }

    if (!*ctx) {
        VIGS_LOG_CRITICAL("wglCreateContextAttribsARB/wglCreateContext failed");
        return false;
    }

    if (share_ctx && gl_backend_wgl->base.is_gl_2) {
        if (!gl_backend_wgl->wglShareLists(share_ctx, *ctx)) {
            VIGS_LOG_CRITICAL("wglShareLists failed");
            gl_backend_wgl->wglDeleteContext(*ctx);
            *ctx = NULL;
            return false;
        }
    }

    return true;
}

static bool vigs_gl_backend_wgl_has_current(struct vigs_gl_backend *gl_backend)
{
    struct vigs_gl_backend_wgl *gl_backend_wgl =
        (struct vigs_gl_backend_wgl*)gl_backend;

    return gl_backend_wgl->wglGetCurrentContext() != NULL;
}

static bool vigs_gl_backend_wgl_make_current(struct vigs_gl_backend *gl_backend,
                                             bool enable)
{
    struct vigs_gl_backend_wgl *gl_backend_wgl =
        (struct vigs_gl_backend_wgl*)gl_backend;

    if (enable) {
        if (!gl_backend_wgl->wglMakeCurrent(gl_backend_wgl->sfc_dc, gl_backend_wgl->ctx)) {
            VIGS_LOG_CRITICAL("wglMakeCurrent failed");
            return false;
        }
    } else {
        if (!gl_backend_wgl->wglMakeCurrent(NULL, NULL)) {
            VIGS_LOG_CRITICAL("wglMakeCurrent failed");
            return false;
        }
    }

    return true;
}

static bool vigs_gl_backend_wgl_read_pixels_make_current(struct vigs_gl_backend *gl_backend,
                                                         bool enable)
{
    struct vigs_gl_backend_wgl *gl_backend_wgl =
        (struct vigs_gl_backend_wgl*)gl_backend;

    if (enable) {
        if (!gl_backend_wgl->wglMakeCurrent(gl_backend_wgl->read_pixels_sfc_dc, gl_backend_wgl->read_pixels_ctx)) {
            VIGS_LOG_CRITICAL("wglMakeCurrent failed");
            return false;
        }
    } else {
        if (!gl_backend_wgl->wglMakeCurrent(NULL, NULL)) {
            VIGS_LOG_CRITICAL("wglMakeCurrent failed");
            return false;
        }
    }

    return true;
}

static void vigs_gl_backend_wgl_destroy(struct vigs_backend *backend)
{
    struct vigs_gl_backend_wgl *gl_backend_wgl = (struct vigs_gl_backend_wgl*)backend;

    vigs_gl_backend_cleanup(&gl_backend_wgl->base);

    gl_backend_wgl->wglDeleteContext(gl_backend_wgl->ctx);
    gl_backend_wgl->wglDeleteContext(gl_backend_wgl->read_pixels_ctx);
    gl_backend_wgl->wglReleasePbufferDCARB(gl_backend_wgl->sfc, gl_backend_wgl->sfc_dc);
    gl_backend_wgl->wglDestroyPbufferARB(gl_backend_wgl->sfc);
    gl_backend_wgl->wglReleasePbufferDCARB(gl_backend_wgl->read_pixels_sfc, gl_backend_wgl->read_pixels_sfc_dc);
    gl_backend_wgl->wglDestroyPbufferARB(gl_backend_wgl->read_pixels_sfc);

    ReleaseDC(gl_backend_wgl->win, gl_backend_wgl->dc);
    DestroyWindow(gl_backend_wgl->win);

    FreeLibrary(gl_backend_wgl->handle);

    vigs_backend_cleanup(&gl_backend_wgl->base.base);

    g_free(gl_backend_wgl);

    UnregisterClassA((LPCTSTR)VIGS_WGL_WIN_CLASS, NULL);

    VIGS_LOG_DEBUG("destroyed");
}

struct vigs_backend *vigs_gl_backend_create(void *display)
{
    WNDCLASSEXA vigs_win_class;
    HWND tmp_win = NULL;
    HDC tmp_dc = NULL;
    HGLRC tmp_ctx = NULL;
    int config_id = 0;
    PIXELFORMATDESCRIPTOR tmp_pixfmt = {
        .nSize = sizeof(PIXELFORMATDESCRIPTOR),
        .nVersion = 1,
        .dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
        .iPixelType = PFD_TYPE_RGBA,
        .cColorBits = 32,
        .cDepthBits = 24,
        .cStencilBits = 8,
        .iLayerType = PFD_MAIN_PLANE,
    };
    const char *ext_str = NULL;
    struct vigs_gl_backend_wgl *gl_backend_wgl = NULL;

    vigs_win_class.cbSize = sizeof(WNDCLASSEXA);
    vigs_win_class.style = 0;
    vigs_win_class.lpfnWndProc = &DefWindowProcA;
    vigs_win_class.cbClsExtra = 0;
    vigs_win_class.cbWndExtra = 0;
    vigs_win_class.hInstance = NULL;
    vigs_win_class.hIcon = NULL;
    vigs_win_class.hCursor = NULL;
    vigs_win_class.hbrBackground = NULL;
    vigs_win_class.lpszMenuName =  NULL;
    vigs_win_class.lpszClassName = VIGS_WGL_WIN_CLASS;
    vigs_win_class.hIconSm = NULL;

    if (!RegisterClassExA(&vigs_win_class)) {
        VIGS_LOG_CRITICAL("Unable to register window class");
        return NULL;
    }

    gl_backend_wgl = g_malloc0(sizeof(*gl_backend_wgl));

    vigs_backend_init(&gl_backend_wgl->base.base,
                      &gl_backend_wgl->base.ws_info.base);

    gl_backend_wgl->handle = LoadLibraryA("opengl32");

    if (!gl_backend_wgl->handle) {
        VIGS_LOG_CRITICAL("Unable to load opengl32.dll");
        goto fail;
    }

    VIGS_WGL_GET_PROC(PFNWGLCREATECONTEXTPROC, wglCreateContext, fail);
    VIGS_WGL_GET_PROC(PFNWGLDELETECONTEXTPROC, wglDeleteContext, fail);
    VIGS_WGL_GET_PROC(PFNWGLGETPROCADDRESSPROC, wglGetProcAddress, fail);
    VIGS_WGL_GET_PROC(PFNWGLMAKECURRENTPROC, wglMakeCurrent, fail);
    VIGS_WGL_GET_PROC(PFNWGLGETCURRENTCONTEXTPROC, wglGetCurrentContext, fail);
    VIGS_WGL_GET_PROC(PFNWGLSHARELISTSPROC, wglShareLists, fail);

    tmp_win = CreateWindow(VIGS_WGL_WIN_CLASS, "VIGSWin",
                           WS_DISABLED | WS_POPUP,
                           0, 0, 1, 1, NULL, NULL, 0, 0);

    if (!tmp_win) {
        VIGS_LOG_CRITICAL("Unable to create window");
        goto fail;
    }

    tmp_dc = GetDC(tmp_win);

    if (!tmp_dc) {
        VIGS_LOG_CRITICAL("Unable to get window DC");
        goto fail;
    }

    config_id = ChoosePixelFormat(tmp_dc, &tmp_pixfmt);

    if (!config_id) {
        VIGS_LOG_CRITICAL("ChoosePixelFormat failed");
        goto fail;
    }

    if (!SetPixelFormat(tmp_dc, config_id, &tmp_pixfmt)) {
        VIGS_LOG_CRITICAL("SetPixelFormat failed");
        goto fail;
    }

    tmp_ctx = gl_backend_wgl->wglCreateContext(tmp_dc);
    if (!tmp_ctx) {
        VIGS_LOG_CRITICAL("wglCreateContext failed");
        goto fail;
    }

    if (!gl_backend_wgl->wglMakeCurrent(tmp_dc, tmp_ctx)) {
        VIGS_LOG_CRITICAL("wglMakeCurrent failed");
        goto fail;
    }

    /*
     * WGL extensions couldn't be queried by glGetString(), we need to use
     * wglGetExtensionsStringARB or wglGetExtensionsStringEXT for this, which
     * themselves are extensions
     */
    gl_backend_wgl->wglGetExtensionsStringARB = (PFNWGLGETEXTENSIONSSTRINGARBPROC)
        gl_backend_wgl->wglGetProcAddress((LPCSTR)"wglGetExtensionsStringARB");
    gl_backend_wgl->wglGetExtensionsStringEXT = (PFNWGLGETEXTENSIONSSTRINGEXTPROC)
        gl_backend_wgl->wglGetProcAddress((LPCSTR)"wglGetExtensionsStringEXT");

    if (gl_backend_wgl->wglGetExtensionsStringARB) {
        ext_str = gl_backend_wgl->wglGetExtensionsStringARB(tmp_dc);
    } else if (gl_backend_wgl->wglGetExtensionsStringEXT) {
        ext_str = gl_backend_wgl->wglGetExtensionsStringEXT();
    }

    if (!ext_str) {
        VIGS_LOG_CRITICAL("Unable to obtain WGL extension string");
        goto fail;
    }

    VIGS_WGL_GET_EXT_PROC(WGL_ARB_pbuffer, PFNWGLCREATEPBUFFERARBPROC, wglCreatePbufferARB);
    VIGS_WGL_GET_EXT_PROC(WGL_ARB_pbuffer, PFNWGLGETPBUFFERDCARBPROC, wglGetPbufferDCARB);
    VIGS_WGL_GET_EXT_PROC(WGL_ARB_pbuffer, PFNWGLRELEASEPBUFFERDCARBPROC, wglReleasePbufferDCARB);
    VIGS_WGL_GET_EXT_PROC(WGL_ARB_pbuffer, PFNWGLDESTROYPBUFFERARBPROC, wglDestroyPbufferARB);
    VIGS_WGL_GET_EXT_PROC(WGL_ARB_pixel_format, PFNWGLCHOOSEPIXELFORMATARBPROC, wglChoosePixelFormatARB);
    VIGS_WGL_GET_EXT_PROC(WGL_ARB_create_context, PFNWGLCREATECONTEXTATTRIBSARBPROC, wglCreateContextAttribsARB);

    VIGS_GL_GET_PROC(GenTextures, glGenTextures);
    VIGS_GL_GET_PROC(DeleteTextures, glDeleteTextures);
    VIGS_GL_GET_PROC(BindTexture, glBindTexture);
    VIGS_GL_GET_PROC(CullFace, glCullFace);
    VIGS_GL_GET_PROC(TexParameterf, glTexParameterf);
    VIGS_GL_GET_PROC(TexParameterfv, glTexParameterfv);
    VIGS_GL_GET_PROC(TexParameteri, glTexParameteri);
    VIGS_GL_GET_PROC(TexParameteriv, glTexParameteriv);
    VIGS_GL_GET_PROC(TexImage2D, glTexImage2D);
    VIGS_GL_GET_PROC(TexSubImage2D, glTexSubImage2D);
    VIGS_GL_GET_PROC(Clear, glClear);
    VIGS_GL_GET_PROC(ClearColor, glClearColor);
    VIGS_GL_GET_PROC(Disable, glDisable);
    VIGS_GL_GET_PROC(Enable, glEnable);
    VIGS_GL_GET_PROC(Finish, glFinish);
    VIGS_GL_GET_PROC(Flush, glFlush);
    VIGS_GL_GET_PROC(PixelStorei, glPixelStorei);
    VIGS_GL_GET_PROC(ReadPixels, glReadPixels);
    VIGS_GL_GET_PROC(Viewport, glViewport);
    VIGS_GL_GET_PROC(GetIntegerv, glGetIntegerv);
    VIGS_GL_GET_PROC(GetString, glGetString);
    VIGS_GL_GET_PROC(DrawArrays, glDrawArrays);
    VIGS_GL_GET_PROC(GenBuffers, glGenBuffers);
    VIGS_GL_GET_PROC(DeleteBuffers, glDeleteBuffers);
    VIGS_GL_GET_PROC(BindBuffer, glBindBuffer);
    VIGS_GL_GET_PROC(BufferData, glBufferData);
    VIGS_GL_GET_PROC(BufferSubData, glBufferSubData);
    VIGS_GL_GET_PROC(MapBuffer, glMapBuffer);
    VIGS_GL_GET_PROC(UnmapBuffer, glUnmapBuffer);
    VIGS_GL_GET_PROC(CreateProgram, glCreateProgram);
    VIGS_GL_GET_PROC(CreateShader, glCreateShader);
    VIGS_GL_GET_PROC(CompileShader, glCompileShader);
    VIGS_GL_GET_PROC(AttachShader, glAttachShader);
    VIGS_GL_GET_PROC(LinkProgram, glLinkProgram);
    VIGS_GL_GET_PROC(GetProgramiv, glGetProgramiv);
    VIGS_GL_GET_PROC(GetProgramInfoLog, glGetProgramInfoLog);
    VIGS_GL_GET_PROC(GetShaderiv, glGetShaderiv);
    VIGS_GL_GET_PROC(GetShaderInfoLog, glGetShaderInfoLog);
    VIGS_GL_GET_PROC(DetachShader, glDetachShader);
    VIGS_GL_GET_PROC(DeleteProgram, glDeleteProgram);
    VIGS_GL_GET_PROC(DeleteShader, glDeleteShader);
    VIGS_GL_GET_PROC(DisableVertexAttribArray, glDisableVertexAttribArray);
    VIGS_GL_GET_PROC(EnableVertexAttribArray, glEnableVertexAttribArray);
    VIGS_GL_GET_PROC(ShaderSource, glShaderSource);
    VIGS_GL_GET_PROC(UseProgram, glUseProgram);
    VIGS_GL_GET_PROC(GetAttribLocation, glGetAttribLocation);
    VIGS_GL_GET_PROC(GetUniformLocation, glGetUniformLocation);
    VIGS_GL_GET_PROC(VertexAttribPointer, glVertexAttribPointer);
    VIGS_GL_GET_PROC(Uniform2fv, glUniform2fv);
    VIGS_GL_GET_PROC(Uniform4fv, glUniform4fv);
    VIGS_GL_GET_PROC(UniformMatrix4fv, glUniformMatrix4fv);

    VIGS_GL_GET_PROC_OPTIONAL(MapBufferRange, glMapBufferRange);

    if (!vigs_gl_backend_wgl_check_gl_version(gl_backend_wgl,
                                              &gl_backend_wgl->base.is_gl_2)) {
        goto fail;
    }

    if (gl_backend_wgl->base.is_gl_2) {
        VIGS_GL_GET_PROC(GenFramebuffers, glGenFramebuffersEXT);
        VIGS_GL_GET_PROC(GenRenderbuffers, glGenRenderbuffersEXT);
        VIGS_GL_GET_PROC(DeleteFramebuffers, glDeleteFramebuffersEXT);
        VIGS_GL_GET_PROC(DeleteRenderbuffers, glDeleteRenderbuffersEXT);
        VIGS_GL_GET_PROC(BindFramebuffer, glBindFramebufferEXT);
        VIGS_GL_GET_PROC(BindRenderbuffer, glBindRenderbufferEXT);
        VIGS_GL_GET_PROC(RenderbufferStorage, glRenderbufferStorageEXT);
        VIGS_GL_GET_PROC(FramebufferRenderbuffer, glFramebufferRenderbufferEXT);
        VIGS_GL_GET_PROC(FramebufferTexture2D, glFramebufferTexture2DEXT);
    } else {
        VIGS_GL_GET_PROC(GenFramebuffers, glGenFramebuffers);
        VIGS_GL_GET_PROC(GenRenderbuffers, glGenRenderbuffers);
        VIGS_GL_GET_PROC(DeleteFramebuffers, glDeleteFramebuffers);
        VIGS_GL_GET_PROC(DeleteRenderbuffers, glDeleteRenderbuffers);
        VIGS_GL_GET_PROC(BindFramebuffer, glBindFramebuffer);
        VIGS_GL_GET_PROC(BindRenderbuffer, glBindRenderbuffer);
        VIGS_GL_GET_PROC(RenderbufferStorage, glRenderbufferStorage);
        VIGS_GL_GET_PROC(FramebufferRenderbuffer, glFramebufferRenderbuffer);
        VIGS_GL_GET_PROC(FramebufferTexture2D, glFramebufferTexture2D);
        VIGS_GL_GET_PROC(GenVertexArrays, glGenVertexArrays);
        VIGS_GL_GET_PROC(BindVertexArray, glBindVertexArray);
        VIGS_GL_GET_PROC(DeleteVertexArrays, glDeleteVertexArrays);
    }

    gl_backend_wgl->wglMakeCurrent(NULL, NULL);
    gl_backend_wgl->wglDeleteContext(tmp_ctx);
    tmp_ctx = NULL;
    ReleaseDC(tmp_win, tmp_dc);
    tmp_dc = NULL;
    DestroyWindow(tmp_win);
    tmp_win = NULL;

    gl_backend_wgl->win = CreateWindow(VIGS_WGL_WIN_CLASS, "VIGSWin",
                                       WS_DISABLED | WS_POPUP,
                                       0, 0, 1, 1, NULL, NULL, 0, 0);

    if (!gl_backend_wgl->win) {
        VIGS_LOG_CRITICAL("Unable to create window");
        goto fail;
    }

    gl_backend_wgl->dc = GetDC(gl_backend_wgl->win);

    if (!gl_backend_wgl->dc) {
        VIGS_LOG_CRITICAL("Unable to get window DC");
        goto fail;
    }

    config_id = vigs_gl_backend_wgl_choose_config(gl_backend_wgl);

    if (!config_id) {
        goto fail;
    }

    if (!vigs_gl_backend_wgl_create_surface(gl_backend_wgl, config_id,
                                            &gl_backend_wgl->sfc,
                                            &gl_backend_wgl->sfc_dc)) {
        goto fail;
    }

    if (!vigs_gl_backend_wgl_create_surface(gl_backend_wgl, config_id,
                                            &gl_backend_wgl->read_pixels_sfc,
                                            &gl_backend_wgl->read_pixels_sfc_dc)) {
        goto fail;
    }

    if (!vigs_gl_backend_wgl_create_context(gl_backend_wgl,
                                            NULL,
                                            &gl_backend_wgl->ctx)) {
        goto fail;
    }

    if (!vigs_gl_backend_wgl_create_context(gl_backend_wgl,
                                            gl_backend_wgl->ctx,
                                            &gl_backend_wgl->read_pixels_ctx)) {
        goto fail;
    }

    gl_backend_wgl->base.base.destroy = &vigs_gl_backend_wgl_destroy;
    gl_backend_wgl->base.has_current = &vigs_gl_backend_wgl_has_current;
    gl_backend_wgl->base.make_current = &vigs_gl_backend_wgl_make_current;
    gl_backend_wgl->base.read_pixels_make_current = &vigs_gl_backend_wgl_read_pixels_make_current;
    gl_backend_wgl->base.ws_info.context = gl_backend_wgl->ctx;

    if (!vigs_gl_backend_init(&gl_backend_wgl->base)) {
        goto fail;
    }

    VIGS_LOG_DEBUG("created");
	
    return &gl_backend_wgl->base.base;

fail:
    if (gl_backend_wgl->ctx) {
        gl_backend_wgl->wglDeleteContext(gl_backend_wgl->ctx);
    }
    if (gl_backend_wgl->read_pixels_ctx) {
        gl_backend_wgl->wglDeleteContext(gl_backend_wgl->read_pixels_ctx);
    }
    if (gl_backend_wgl->sfc_dc) {
        gl_backend_wgl->wglReleasePbufferDCARB(gl_backend_wgl->sfc, gl_backend_wgl->sfc_dc);
    }
    if (gl_backend_wgl->sfc) {
        gl_backend_wgl->wglDestroyPbufferARB(gl_backend_wgl->sfc);
    }
    if (gl_backend_wgl->read_pixels_sfc_dc) {
        gl_backend_wgl->wglReleasePbufferDCARB(gl_backend_wgl->read_pixels_sfc, gl_backend_wgl->read_pixels_sfc_dc);
    }
    if (gl_backend_wgl->read_pixels_sfc) {
        gl_backend_wgl->wglDestroyPbufferARB(gl_backend_wgl->read_pixels_sfc);
    }
    if (gl_backend_wgl->dc) {
        ReleaseDC(gl_backend_wgl->win, gl_backend_wgl->dc);
    }
    if (gl_backend_wgl->win) {
        DestroyWindow(gl_backend_wgl->win);
    }
    if (gl_backend_wgl->wglMakeCurrent && tmp_ctx) {
        gl_backend_wgl->wglMakeCurrent(NULL, NULL);
    }
    if (gl_backend_wgl->wglDeleteContext && tmp_ctx) {
        gl_backend_wgl->wglDeleteContext(tmp_ctx);
    }
    if (tmp_dc) {
        ReleaseDC(tmp_win, tmp_dc);
    }
    if (tmp_win) {
        DestroyWindow(tmp_win);
    }
    if (gl_backend_wgl->handle) {
        FreeLibrary(gl_backend_wgl->handle);
    }

    vigs_backend_cleanup(&gl_backend_wgl->base.base);

    g_free(gl_backend_wgl);

    UnregisterClassA((LPCTSTR)VIGS_WGL_WIN_CLASS, NULL);

    return NULL;
}
