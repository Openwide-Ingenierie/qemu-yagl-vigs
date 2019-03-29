/*
 * yagl
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

#include "yagl_server.h"
#include "yagl_log.h"
#include "yagl_handle_gen.h"
#include "yagl_stats.h"
#include "yagl_process.h"
#include "yagl_thread.h"
#include "yagl_egl_driver.h"
#include "yagl_drivers/gles_ogl/yagl_gles_ogl.h"
#include "yagl_drivers/gles_onscreen/yagl_gles_onscreen.h"
#include "yagl_backends/egl_offscreen/yagl_egl_offscreen.h"
#include "yagl_backends/egl_onscreen/yagl_egl_onscreen.h"
#include "exec/cpu-all.h"
#include "hw/hw.h"
#include "hw/pci/pci.h"
#include <GL/gl.h>
#include "hw/winsys.h"
#include "yagl_gles_driver.h"

#define PCI_VENDOR_ID_YAGL 0x19B1
#define PCI_DEVICE_ID_YAGL 0x1010

#define YAGL_REG_BUFFPTR 0
#define YAGL_REG_TRIGGER 4
#define YAGL_REGS_SIZE   8

#define YAGL_MEM_SIZE 0x1000

#define YAGL_MAX_USERS (YAGL_MEM_SIZE / YAGL_REGS_SIZE)

struct work_queue;

#ifdef _WIN32
extern void *vigs_display;
#else
extern Display *vigs_display;
#endif
extern struct work_queue *vigs_render_queue;
extern struct winsys_interface *vigs_wsi;

struct yagl_user
{
    bool activated;
    yagl_pid process_id;
    yagl_tid thread_id;
};

typedef struct YaGLState
{
    PCIDevice dev;

    MemoryRegion iomem;
    struct yagl_server_state *ss;
    struct yagl_user users[YAGL_MAX_USERS];
} YaGLState;

#define TYPE_YAGL_DEVICE "yagl"

static void yagl_device_operate(YaGLState *s, int user_index, hwaddr buff_pa)
{
    yagl_pid target_pid;
    yagl_tid target_tid;
    hwaddr buff_len = TARGET_PAGE_SIZE;
    uint8_t *buff = NULL;

    YAGL_LOG_FUNC_ENTER(yagl_device_operate,
                        "user_index = %d, buff_pa = 0x%X",
                        user_index,
                        (uint32_t)buff_pa);

    if (!buff_pa && !s->users[user_index].activated) {
        YAGL_LOG_CRITICAL("user %d is not activated", user_index);
        goto out;
    }

    if (buff_pa) {
        buff = cpu_physical_memory_map(buff_pa, &buff_len, false);

        if (!buff || (buff_len != TARGET_PAGE_SIZE)) {
            YAGL_LOG_CRITICAL("cpu_physical_memory_map(read) failed for user %d, buff_pa = 0x%X",
                              user_index,
                              (uint32_t)buff_pa);
            goto out;
        }

        if (s->users[user_index].activated) {
            /*
             * Update user.
             */

            yagl_server_dispatch_update(s->ss,
                                        s->users[user_index].process_id,
                                        s->users[user_index].thread_id,
                                        buff);
        } else {
            /*
             * Activate user.
             */

            if (yagl_server_dispatch_init(s->ss,
                                          buff,
                                          &target_pid,
                                          &target_tid)) {
                s->users[user_index].activated = true;
                s->users[user_index].process_id = target_pid;
                s->users[user_index].thread_id = target_tid;

                YAGL_LOG_DEBUG("user %d activated", user_index);

                /*
                 * The buff is now owned by client.
                 */
                buff = NULL;
            }
        }
    } else {
        /*
         * Deactivate user.
         */

        yagl_server_dispatch_exit(s->ss,
                                  s->users[user_index].process_id,
                                  s->users[user_index].thread_id);

        memset(&s->users[user_index], 0, sizeof(s->users[user_index]));

        YAGL_LOG_DEBUG("user %d deactivated", user_index);
    }

out:
    if (buff) {
        cpu_physical_memory_unmap(buff,
                                  TARGET_PAGE_SIZE,
                                  0,
                                  TARGET_PAGE_SIZE);
    }

    YAGL_LOG_FUNC_EXIT(NULL);
}

static void yagl_device_trigger(YaGLState *s, int user_index, bool sync)
{
    YAGL_LOG_FUNC_ENTER(yagl_device_trigger, "%d, %d", user_index, sync);

    if (s->users[user_index].activated) {
        yagl_server_dispatch_batch(s->ss,
                                   s->users[user_index].process_id,
                                   s->users[user_index].thread_id,
                                   sync);
    } else {
        YAGL_LOG_CRITICAL("user %d not activated", user_index);
    }

    YAGL_LOG_FUNC_EXIT(NULL);
}

static uint64_t yagl_device_read(void *opaque, hwaddr offset,
                                 unsigned size)
{
    return 0;
}

static void yagl_device_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    YaGLState *s = (YaGLState*)opaque;
    int user_index = (offset / YAGL_REGS_SIZE);
    offset -= user_index * YAGL_REGS_SIZE;

    assert(user_index < YAGL_MAX_USERS);

    if (user_index >= YAGL_MAX_USERS) {
        YAGL_LOG_CRITICAL("bad user index = %d", user_index);
        return;
    }

    switch (offset) {
    case YAGL_REG_BUFFPTR:
        yagl_device_operate(s, user_index, value);
        break;
    case YAGL_REG_TRIGGER:
        yagl_device_trigger(s, user_index, value);
        break;
    default:
        YAGL_LOG_CRITICAL("user %d, bad offset = %d", user_index, offset);
        break;
    }
}

static const MemoryRegionOps yagl_device_ops =
{
    .read = yagl_device_read,
    .write = yagl_device_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static int yagl_device_init(PCIDevice *dev)
{
    YaGLState *s = DO_UPCAST(YaGLState, dev, dev);
    struct yagl_egl_driver *egl_driver = NULL;
    struct yagl_egl_backend *egl_backend = NULL;
    struct yagl_gles_driver *gles_driver = NULL;

    yagl_log_init();

    YAGL_LOG_FUNC_ENTER(yagl_device_init, NULL);

    memory_region_init_io(&s->iomem, OBJECT(s),
                          &yagl_device_ops,
                          s,
                          TYPE_YAGL_DEVICE,
                          YAGL_MEM_SIZE);

    yagl_handle_gen_init();

    egl_driver = yagl_egl_driver_create(vigs_display);

    if (!egl_driver) {
        goto fail;
    }

    gles_driver = yagl_gles_ogl_create(egl_driver->dyn_lib,
                                       egl_driver->gl_version);

    if (!gles_driver) {
        goto fail;
    }

    if (vigs_wsi) {
        egl_backend = yagl_egl_onscreen_create(vigs_wsi,
                                               egl_driver,
                                               gles_driver);
        gles_driver = yagl_gles_onscreen_create(gles_driver);
    } else {
        egl_backend = yagl_egl_offscreen_create(egl_driver, gles_driver);
    }

    if (!egl_backend) {
        goto fail;
    }

    /*
     * Now owned by EGL backend.
     */
    egl_driver = NULL;

    s->ss = yagl_server_state_create(egl_backend, gles_driver,
                                     vigs_render_queue, vigs_wsi);

    /*
     * Owned/destroyed by server state.
     */
    egl_backend = NULL;
    gles_driver = NULL;

    if (!s->ss) {
        goto fail;
    }

    pci_register_bar(&s->dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->iomem);

    YAGL_LOG_FUNC_EXIT(NULL);

    return 0;

fail:
    if (gles_driver) {
        gles_driver->destroy(gles_driver);
    }

    if (egl_driver) {
        egl_driver->destroy(egl_driver);
    }

    yagl_handle_gen_cleanup();

    YAGL_LOG_FUNC_EXIT(NULL);

    yagl_log_cleanup();

    return -1;
}

static void yagl_device_reset(DeviceState *d)
{
    YaGLState *s = container_of(d, YaGLState, dev.qdev);
    int i;

    YAGL_LOG_FUNC_ENTER(yagl_device_reset, NULL);

    yagl_server_reset(s->ss);

    yagl_handle_gen_reset();

    for (i = 0; i < YAGL_MAX_USERS; ++i) {
        memset(&s->users[i], 0, sizeof(s->users[i]));
    }

    YAGL_LOG_FUNC_EXIT(NULL);
}

static void yagl_device_exit(PCIDevice *dev)
{
    YaGLState *s = DO_UPCAST(YaGLState, dev, dev);

    YAGL_LOG_FUNC_ENTER(yagl_device_exit, NULL);

    memory_region_destroy(&s->iomem);

    yagl_server_state_destroy(s->ss);

    yagl_handle_gen_cleanup();

    YAGL_LOG_FUNC_EXIT(NULL);

    yagl_log_cleanup();
}

static void yagl_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->init = yagl_device_init;
    k->exit = yagl_device_exit;
    k->vendor_id = PCI_VENDOR_ID_YAGL;
    k->device_id = PCI_DEVICE_ID_YAGL;
    k->class_id = PCI_CLASS_OTHERS;
    dc->reset = yagl_device_reset;
    dc->desc = "YaGL device";
}

static TypeInfo yagl_device_info =
{
    .name          = TYPE_YAGL_DEVICE,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(YaGLState),
    .class_init    = yagl_class_init,
};

static void yagl_register_types(void)
{
    type_register_static(&yagl_device_info);
}

type_init(yagl_register_types)
