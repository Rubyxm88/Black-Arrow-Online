/*
 * QEMU SMBus Generic Storage Device
 *
 * Copyright (c) 2020 Mike Davis
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/qdev-properties.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/smbus_slave.h"
#include "smbus.h"
#include "qapi/error.h"
#include "hw/loader.h"
#include "migration/vmstate.h"
#include "system/reset.h"
#include "eeprom_generation.h"
#include "xbox-audio-setting.h"

#define TYPE_SMBUS_STORAGE "smbus-storage"
#define SMBUS_STORAGE(obj) OBJECT_CHECK(SMBusStorageDevice, (obj), \
    TYPE_SMBUS_STORAGE)

//#define DEBUG
#ifdef DEBUG
# define DPRINTF(format, ...) printf(format, ## __VA_ARGS__)
#else
# define DPRINTF(format, ...) do { } while (0)
#endif

typedef struct SMBusStorageDevice {
    SMBusDevice smbusdev;
    char* file;
    uint8_t *data;
    uint32_t size;
    uint8_t addr;
    uint32_t offset;
    bool persist;
} SMBusStorageDevice;

static bool smbus_storage_save(SMBusStorageDevice *s)
{
    if (!s->file || !s->persist) {
        return true;
    }
    int fd = qemu_open(s->file, O_WRONLY | O_BINARY, NULL);
    if (fd < 0) {
        DPRINTF("%s: file '%s' could not be opened\n", __func__, s->file);
        return false;
    }

    int wc = write(fd, s->data, s->size);
    if (wc != s->size) {
        DPRINTF( "%s: file '%s' write failure\n", __func__, s->file);
        close(fd);
        return false;
    }
    close(fd);
    return true;
}

/*
 * xemu: the Xbox's audio setting (Stereo, Dolby Surround, Dolby Digital) can
 * be changed from xemu's Sound menu without the dashboard. The kernel reads
 * the EEPROM when the Xbox starts, so a change applies after a reset; the
 * setting the Xbox started with is kept apart for that reason.
 */
#define XBOX_EEPROM_ADDR 0x54
#define EEPROM_USER_CHECKSUM 0x60
#define EEPROM_USER 0x64
#define EEPROM_USER_CHECKED_LEN 0x5C
#define EEPROM_AUDIO_FLAGS 0x98

static SMBusStorageDevice *eeprom_dev;
static uint32_t eeprom_booted_audio;
static bool eeprom_booted_known;

static void eeprom_reset(void *opaque)
{
    SMBusStorageDevice *s = opaque;
    eeprom_booted_audio = ldl_le_p(s->data + EEPROM_AUDIO_FLAGS);
    eeprom_booted_known = true;
}

bool xbox_audio_setting_get(uint32_t *now, uint32_t *booted)
{
    if (!eeprom_dev) {
        return false;
    }
    *now = ldl_le_p(eeprom_dev->data + EEPROM_AUDIO_FLAGS);
    *booted = eeprom_booted_known ? eeprom_booted_audio : *now;
    return true;
}

bool xbox_audio_setting_set(uint32_t flags)
{
    SMBusStorageDevice *s = eeprom_dev;
    if (!s) {
        return false;
    }
    uint32_t old = ldl_le_p(s->data + EEPROM_AUDIO_FLAGS);
    flags = (old & ~XBOX_AUDIO_SETTING_MASK) | (flags & XBOX_AUDIO_SETTING_MASK);
    if (flags == old) {
        return true;
    }

    /* Once: the console's EEPROM as it was before xemu changed it */
    if (s->file) {
        char *backup = g_strdup_printf("%s.before-sound", s->file);
        if (!g_file_test(backup, G_FILE_TEST_EXISTS)) {
            g_file_set_contents(backup, (const gchar *)s->data, s->size,
                                NULL);
        }
        g_free(backup);
    }

    stl_le_p(s->data + EEPROM_AUDIO_FLAGS, flags);
    stl_le_p(s->data + EEPROM_USER_CHECKSUM,
             xbox_eeprom_crc(s->data + EEPROM_USER, EEPROM_USER_CHECKED_LEN));
    return smbus_storage_save(s);
}

static void smbus_storage_realize(DeviceState *dev, Error **errp)
{
    SMBusStorageDevice *s = SMBUS_STORAGE(dev);
    qdev_prop_set_uint8(dev, "address", s->addr);
    s->data = g_malloc0(s->size);
    s->offset = 0;

    if (s->file) {
        int size = get_image_size(s->file, NULL);
        if (size != s->size) {
            error_setg(errp, "%s: file '%s' size of %d, expected %d\n",
                __func__, s->file, size, s->size);
            return;
        }

        int fd = qemu_open(s->file, O_RDONLY | O_BINARY, NULL);
        if (fd < 0) {
            error_setg(errp, "%s: file '%s' could not be opened\n",
                __func__, s->file);
            return;
        }

        int rc = read(fd, s->data, s->size);
        if (rc != s->size) {
            error_setg(errp, "%s: file '%s' read failure\n", __func__, s->file);
            close(fd);
            return;
        }
        close(fd);
    } else {
        error_setg(errp, "%s: file unspecified\n", __func__);
    }

    if (s->addr == XBOX_EEPROM_ADDR) {
        eeprom_dev = s;
        qemu_register_reset(eeprom_reset, s);
    }
}

static int smbus_storage_write_data(SMBusDevice *dev, uint8_t *buf, uint8_t len)
{
    SMBusStorageDevice *s = SMBUS_STORAGE(dev);

    DPRINTF("%s: addr=0x%02x cmd=0x%02x val=0x%02x\n",
        __func__, s->addr, buf[0], buf[1]);

    /* len is guaranteed to be > 0 */
    s->offset = buf[0];
    buf++;
    len--;

    bool changed = false;
    for (; len > 0; len--) {
        changed = true;
        s->data[s->offset] = *buf++;
        DPRINTF("%s: addr=0x%02x off=0x%02x, data=0x%02x\n",
            __func__, s->addr, s->offset, s->data[s->offset]);
        s->offset = (s->offset + 1) % s->size;
    }

    if (changed && !smbus_storage_save(s)) {
        return -1;
    }

    return 0;
}

static uint8_t smbus_storage_receive_byte(SMBusDevice *dev)
{
    SMBusStorageDevice *s = SMBUS_STORAGE(dev);

    uint8_t val = s->data[s->offset];
    DPRINTF("%s: addr=0x%02x off=0x%02x val=0x%02x\n",
        __func__, s->addr, s->offset, val);
    s->offset = (s->offset + 1) % s->size;

    return val;
}

static const VMStateDescription vmstate_smbus_storage = {
    .name = "smbus-storage",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields      = (VMStateField[]) {
        VMSTATE_SMBUS_DEVICE(smbusdev, SMBusStorageDevice),
        VMSTATE_VBUFFER_UINT32(data, SMBusStorageDevice, 1, NULL, size),
        VMSTATE_UINT32(offset, SMBusStorageDevice),
        VMSTATE_END_OF_LIST()
    }
};

// default xbox eeprom with persistence
static const Property smbus_storage_props[] = {
    DEFINE_PROP_UINT8("addr", SMBusStorageDevice, addr, 0x54),
    DEFINE_PROP_UINT32("size", SMBusStorageDevice, size, 256),
    DEFINE_PROP_BOOL("persist", SMBusStorageDevice, persist, true),
    DEFINE_PROP_STRING("file", SMBusStorageDevice, file),
};

static void smbus_storage_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SMBusDeviceClass *sc = SMBUS_DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_smbus_storage;
    dc->realize = smbus_storage_realize;
    sc->receive_byte = smbus_storage_receive_byte;
    sc->write_data = smbus_storage_write_data;
    device_class_set_props(dc, smbus_storage_props);
}

static TypeInfo smbus_storage_info = {
    .name = TYPE_SMBUS_STORAGE,
    .parent = TYPE_SMBUS_DEVICE,
    .instance_size = sizeof(SMBusStorageDevice),
    .class_init = smbus_storage_class_init,
};

static void smbus_storage_register_devices(void)
{
    type_register_static(&smbus_storage_info);
}

type_init(smbus_storage_register_devices)
