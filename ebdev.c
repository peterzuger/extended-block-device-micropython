/**
 * @file   extended-block-device-micropython/ebdev.c
 * @author Peter Züger
 * @date   08.12.2023
 * @brief  adapter for providing the extended blockdev interface for normal block devices
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2023 Peter Züger
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "py/mpconfig.h"

#if defined(MODULE_EXTENDED_BLOCKDEV_ENABLED) && MODULE_EXTENDED_BLOCKDEV_ENABLED == 1

#include "py/obj.h"
#include "py/runtime.h"
#include "py/mperrno.h"
#include "extmod/vfs.h"

#include <stdio.h>

typedef enum{
    CLEAN,
    DIRTY
}cache_state_t;

typedef struct _extended_blockdev_EBDev_obj_t{
    // base represents some basic information, like type
    mp_obj_base_t base;

    mp_obj_t bdev;
    size_t start_block;
    size_t block_count;
    size_t block_size;

    mp_obj_t readblocks[4];
    mp_obj_t writeblocks[4];
    mp_obj_t ioctl[4];

    cache_state_t cache_state;
    size_t cache_block;
    vstr_t cache;
}extended_blockdev_EBDev_obj_t;

mp_obj_t extended_blockdev_EBDev_make_new(const mp_obj_type_t* type, size_t n_args, size_t n_kw, const mp_obj_t* args);
static void extended_blockdev_EBDev_print(const mp_print_t* print, mp_obj_t self_in, mp_print_kind_t kind);
static mp_obj_t extended_blockdev_EBDev_readblocks(size_t n_args, const mp_obj_t* args);
static mp_obj_t extended_blockdev_EBDev_writeblocks(size_t n_args, const mp_obj_t* args);
static mp_obj_t extended_blockdev_EBDev_ioctl(mp_obj_t self_in, mp_obj_t op_in, mp_obj_t arg_in);

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(extended_blockdev_EBDev_readblocks_fun_obj, 3, 4, extended_blockdev_EBDev_readblocks);
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(extended_blockdev_EBDev_writeblocks_fun_obj, 3, 4, extended_blockdev_EBDev_writeblocks);
static MP_DEFINE_CONST_FUN_OBJ_3(extended_blockdev_EBDev_ioctl_fun_obj, extended_blockdev_EBDev_ioctl);

static const mp_rom_map_elem_t extended_blockdev_EBDev_locals_dict_table[] = {
    // class methods
    { MP_ROM_QSTR(MP_QSTR_readblocks), MP_ROM_PTR(&extended_blockdev_EBDev_readblocks_fun_obj) },
    { MP_ROM_QSTR(MP_QSTR_writeblocks), MP_ROM_PTR(&extended_blockdev_EBDev_writeblocks_fun_obj) },
    { MP_ROM_QSTR(MP_QSTR_ioctl), MP_ROM_PTR(&extended_blockdev_EBDev_ioctl_fun_obj) },
};
static MP_DEFINE_CONST_DICT(extended_blockdev_EBDev_locals_dict, extended_blockdev_EBDev_locals_dict_table);


MP_DEFINE_CONST_OBJ_TYPE(
    extended_blockdev_EBDev_type,
    MP_QSTR_EBDev,
    MP_TYPE_FLAG_NONE,
    print, extended_blockdev_EBDev_print,
    make_new, extended_blockdev_EBDev_make_new,
    locals_dict, &extended_blockdev_EBDev_locals_dict
    );

/**
 * Python: extended_blockdev.EBDev(bdev, start=0, len=None)
 * @param bdev
 * @param start
 * @param len
 */
mp_obj_t extended_blockdev_EBDev_make_new(const mp_obj_type_t* type,
                                          size_t n_args,
                                          size_t n_kw,
                                          const mp_obj_t* args){
    mp_arg_check_num(n_args, n_kw, 1, 3, false);

    // raises MemoryError
    extended_blockdev_EBDev_obj_t* self = mp_obj_malloc(extended_blockdev_EBDev_obj_t, type);

    self->bdev = args[0];

    mp_load_method(self->bdev, MP_QSTR_readblocks, self->readblocks);
    mp_load_method_maybe(self->bdev, MP_QSTR_writeblocks, self->writeblocks);
    mp_load_method(self->bdev, MP_QSTR_ioctl, self->ioctl);

    // get blocksize (usually fixed at 512 by micropython)
    self->ioctl[2] = MP_OBJ_NEW_SMALL_INT(MP_BLOCKDEV_IOCTL_BLOCK_SIZE); // op
    self->ioctl[3] = MP_OBJ_NEW_SMALL_INT(0); // arg

    // raises TypeError, OverflowError
    size_t blksize = mp_obj_get_uint(mp_call_method_n_kw(2, 0, self->ioctl));

    // get the size of the device (in number of blocks)
    self->ioctl[2] = MP_OBJ_NEW_SMALL_INT(MP_BLOCKDEV_IOCTL_BLOCK_COUNT); // op
    self->ioctl[3] = MP_OBJ_NEW_SMALL_INT(0); // arg

    // raises TypeError, OverflowError
    size_t blkcnt = mp_obj_get_uint(mp_call_method_n_kw(2, 0, self->ioctl));


    self->start_block = 0;
    self->block_count = blkcnt;
    self->block_size = blksize;
    self->cache_state = CLEAN;
    self->cache_block = (size_t)(-1); // no block cached
    vstr_init(&self->cache, self->block_size);

    if(n_args >= 2){
        // raises TypeError, OverflowError
        uint64_t start_bytes = mp_obj_get_ll(args[1]);

        // check start is a multiple of blocksize
        if((start_bytes % blksize) != 0){
            mp_raise_ValueError(MP_ERROR_TEXT("start must be a multiple of blocksize"));
        }

        self->start_block = start_bytes / blksize;

        if(self->start_block >= self->block_count){
            mp_raise_ValueError(MP_ERROR_TEXT("device overflow"));
        }

        self->block_count -= self->start_block; // shorten device
    }

    if((n_args == 3) && (args[2] != mp_const_none)){
        // raises TypeError, OverflowError
        uint64_t len_bytes = mp_obj_get_ll(args[2]);

        // check length is a multiple of blocksize
        if((len_bytes % blksize) != 0){
            mp_raise_ValueError(MP_ERROR_TEXT("len must be a multiple of blocksize"));
        }

        blkcnt = len_bytes / blksize;

        // check device overflow
        if(blkcnt > self->block_count){
            mp_raise_ValueError(MP_ERROR_TEXT("device overflow"));
        }

        self->block_count = blkcnt;
    }

    return MP_OBJ_FROM_PTR(self);
}

/**
 * Python: print(extended_blockdev.EBDev())
 * @param obj
 */
static void extended_blockdev_EBDev_print(const mp_print_t* print,
                                          mp_obj_t self_in, mp_print_kind_t kind){
    extended_blockdev_EBDev_obj_t* self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "EBDev(start=%d, len=%d)", self->start_block, self->block_count);
}

static int extended_blockdev_EBDev_flush(extended_blockdev_EBDev_obj_t* self){
    if(self->cache_state == DIRTY){
        self->writeblocks[2] = mp_obj_new_int_from_uint(self->start_block + self->cache_block);
        self->writeblocks[3] = mp_obj_new_bytearray_by_ref(self->block_size, self->cache.buf);

        mp_obj_t ret = mp_call_method_n_kw(2, 0, self->writeblocks);

        if(ret == mp_const_false){
            return -MP_EIO;
        }

        if((ret != mp_const_true) && (ret != mp_const_none)){
            int i = MP_OBJ_SMALL_INT_VALUE(ret);
            if(i != 0){
                return i > 0 ? (-MP_EINVAL) : i;
            }
        }

        self->cache_state = CLEAN;
    }

    return 0;
}

static int extended_blockdev_EBDev_read(extended_blockdev_EBDev_obj_t* self, size_t block){
    self->readblocks[2] = mp_obj_new_int_from_uint(self->start_block + block);
    self->readblocks[3] = mp_obj_new_bytearray_by_ref(self->block_size, self->cache.buf);

    mp_obj_t ret = mp_call_method_n_kw(2, 0, self->readblocks);

    if(ret == mp_const_false){
        return -MP_EIO;
    }

    if((ret != mp_const_true) && (ret != mp_const_none)){
        int i = mp_obj_get_uint(ret);
        if(i != 0){
            return i > 0 ? (-MP_EINVAL) : i;
        }
    }

    self->cache_block = block;

    return 0;
}

/**
 * Python: extended_blockdev.EBDev.readblocks(self, block, buf, offset)
 *
 * def readblocks(self, block, buf, offset=None):
 *     if offset:
 *         return -EINVAL
 *     if self.bdev.readblocks(block, buf):
 *         return 0
 *     return EPERM
 *
 * @param self
 * @param block
 * @param buf
 * @param offset
 */
static mp_obj_t extended_blockdev_EBDev_readblocks(size_t n_args, const mp_obj_t* args){
    extended_blockdev_EBDev_obj_t* self = MP_OBJ_TO_PTR(args[0]);

    // raises TypeError, OverflowError
    size_t block = mp_obj_get_uint(args[1]);

    // cache miss
    if(block != self->cache_block){
        if(block >= self->block_count){
            return MP_OBJ_NEW_SMALL_INT(-MP_EINVAL);
        }

        int ret = extended_blockdev_EBDev_flush(self);
        if(ret != 0){
            return MP_OBJ_NEW_SMALL_INT(ret);
        }

        ret = extended_blockdev_EBDev_read(self, block);
        if(ret != 0){
            return MP_OBJ_NEW_SMALL_INT(ret);
        }
    }

    mp_uint_t offset = 0;
    if(n_args >= 4 && args[3] != mp_const_none){
        // raises TypeError, OverflowError
        offset = mp_obj_get_uint(args[3]);
        if(offset >= self->block_size){
            return MP_OBJ_NEW_SMALL_INT(-MP_EINVAL);
        }
    }

    mp_buffer_info_t bufinfo;

    // raises TypeError
    mp_get_buffer_raise(args[2], &bufinfo, MP_BUFFER_READ);

    if(bufinfo.len > (self->block_size - offset)){
        // if the buffer is larger than our cache we can still just call the
        // underlying blockdevice, but only if the offset is zero.
        if(offset == 0){
            // still do the start_block offset
            self->readblocks[2] = mp_obj_new_int_from_uint(self->start_block + block);
            self->readblocks[3] = args[2];

            return mp_call_method_n_kw(2, 0, self->readblocks);
        }

        return MP_OBJ_NEW_SMALL_INT(-MP_EINVAL);
    }

    memcpy(bufinfo.buf, self->cache.buf + offset, bufinfo.len);

    return MP_OBJ_NEW_SMALL_INT(0);
}

/**
 * Python: extended_blockdev.EBDev.writeblocks(self, block, buf, offset)
 *
 * def writeblocks(self, block, buf, offset=None):
 *     if offset:
 *         return -EINVAL
 *     if self.bdev.writeblocks(block, buf):
 *         return 0
 *     return EPERM
 *
 * @param self
 * @param block
 * @param buf
 * @param offset
 */
static mp_obj_t extended_blockdev_EBDev_writeblocks(size_t n_args, const mp_obj_t* args){
    extended_blockdev_EBDev_obj_t* self = MP_OBJ_TO_PTR(args[0]);

    if(self->writeblocks[0] == MP_OBJ_NULL){
        // read-only block device
        return MP_OBJ_NEW_SMALL_INT(-MP_EROFS);
    }

    // raises TypeError, OverflowError
    size_t block = mp_obj_get_uint(args[1]);

    mp_uint_t offset = 0;
    if(n_args >= 4 && args[3] != mp_const_none){
        // raises TypeError, OverflowError
        offset = mp_obj_get_uint(args[3]);
        if(offset >= self->block_size){
            return MP_OBJ_NEW_SMALL_INT(-MP_EINVAL);
        }
    }

    mp_buffer_info_t bufinfo;

    // raises TypeError
    mp_get_buffer_raise(args[2], &bufinfo, MP_BUFFER_READ);

    // cache miss
    if(block != self->cache_block){
        if(block >= self->block_count){
            return MP_OBJ_NEW_SMALL_INT(-MP_EINVAL);
        }

        int ret = extended_blockdev_EBDev_flush(self);
        if(ret != 0){
            return MP_OBJ_NEW_SMALL_INT(ret);
        }

        if(offset == 0 && bufinfo.len >= self->block_size){
            // we will write the entire cache block
            // no need to read
            self->cache_block = block;
        }else{
            ret = extended_blockdev_EBDev_read(self, block);
            if(ret != 0){
                return MP_OBJ_NEW_SMALL_INT(ret);
            }
        }
    }

    if(bufinfo.len > (self->block_size - offset)){
        // if the buffer is larger than our cache we can still just call the
        // underlying blockdevice, but only if the offset is zero.
        if(offset == 0){
            memcpy(self->cache.buf, bufinfo.buf, self->block_size);
            // self->cache_state = CLEAN;

            // still do the start_block offset
            self->writeblocks[2] = mp_obj_new_int_from_uint(self->start_block + block);
            self->writeblocks[3] = args[2];

            return mp_call_method_n_kw(2, 0, self->writeblocks);
        }

        return MP_OBJ_NEW_SMALL_INT(-MP_EINVAL);
    }

    self->cache_state = DIRTY;
    memcpy(self->cache.buf + offset, bufinfo.buf, bufinfo.len);

    return MP_OBJ_NEW_SMALL_INT(0);
}

/**
 * Python: extended_blockdev.EBDev.ioctl(self, op, arg)
 *
 * def ioctl(self, op, arg):
 *     if op == 6: # ignore erase
 *         return 0
 *     if op == 4: # intercept block count
 *         return self.block_count
 *     return self.bdev.ioctl(op, arg)
 *
 * @param self
 * @param op
 * @param arg
 */
static mp_obj_t extended_blockdev_EBDev_ioctl(mp_obj_t self_in, mp_obj_t op_in, mp_obj_t arg_in){
    extended_blockdev_EBDev_obj_t* self = MP_OBJ_TO_PTR(self_in);

    // raises TypeError, OverflowError
    size_t op = mp_obj_get_uint(op_in);

    if(op == MP_BLOCKDEV_IOCTL_BLOCK_ERASE){
        return MP_OBJ_NEW_SMALL_INT(0);
    }

    if(op == MP_BLOCKDEV_IOCTL_BLOCK_COUNT){
        return MP_OBJ_NEW_SMALL_INT(self->block_count);
    }

    if(op == MP_BLOCKDEV_IOCTL_SYNC){
        int ret = extended_blockdev_EBDev_flush(self);
        if(ret != 0){
            return MP_OBJ_NEW_SMALL_INT(ret);
        }
    }

    self->ioctl[2] = op_in;
    self->ioctl[3] = arg_in;
    return mp_call_method_n_kw(2, 0, self->ioctl);
}


static const mp_rom_map_elem_t extended_blockdev_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_extended_blockdev) },

    { MP_ROM_QSTR(MP_QSTR_EBDev), MP_ROM_PTR(&extended_blockdev_EBDev_type) },
};

static MP_DEFINE_CONST_DICT(
    mp_module_extended_blockdev_globals,
    extended_blockdev_globals_table
    );

const mp_obj_module_t mp_module_extended_blockdev = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&mp_module_extended_blockdev_globals,
};

MP_REGISTER_MODULE(MP_QSTR_extended_blockdev, mp_module_extended_blockdev);

#endif /* defined(MODULE_EXTENDED_BLOCKDEV_ENABLED) && MODULE_EXTENDED_BLOCKDEV_ENABLED == 1 */
