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

typedef struct _extended_blockdev_EBDev_obj_t{
    // base represents some basic information, like type
    mp_obj_base_t base;

    mp_obj_t bdev;
    size_t start_block;
    size_t block_count;

    mp_obj_t readblocks[4];
    mp_obj_t writeblocks[4];
    mp_obj_t ioctl[4];
}extended_blockdev_EBDev_obj_t;

mp_obj_t extended_blockdev_EBDev_make_new(const mp_obj_type_t* type, size_t n_args, size_t n_kw, const mp_obj_t* args);
STATIC void extended_blockdev_EBDev_print(const mp_print_t* print, mp_obj_t self_in, mp_print_kind_t kind);
STATIC mp_obj_t extended_blockdev_EBDev_readblocks(size_t n_args, const mp_obj_t* args);
STATIC mp_obj_t extended_blockdev_EBDev_writeblocks(size_t n_args, const mp_obj_t* args);
STATIC mp_obj_t extended_blockdev_EBDev_ioctl(mp_obj_t self_in, mp_obj_t op_in, mp_obj_t arg_in);

STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(extended_blockdev_EBDev_readblocks_fun_obj, 3, 4, extended_blockdev_EBDev_readblocks);
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(extended_blockdev_EBDev_writeblocks_fun_obj, 3, 4, extended_blockdev_EBDev_writeblocks);
STATIC MP_DEFINE_CONST_FUN_OBJ_3(extended_blockdev_EBDev_ioctl_fun_obj, extended_blockdev_EBDev_ioctl);

STATIC const mp_rom_map_elem_t extended_blockdev_EBDev_locals_dict_table[] = {
    // class methods
    { MP_ROM_QSTR(MP_QSTR_readblocks), MP_ROM_PTR(&extended_blockdev_EBDev_readblocks_fun_obj) },
    { MP_ROM_QSTR(MP_QSTR_writeblocks), MP_ROM_PTR(&extended_blockdev_EBDev_writeblocks_fun_obj) },
    { MP_ROM_QSTR(MP_QSTR_ioctl), MP_ROM_PTR(&extended_blockdev_EBDev_ioctl_fun_obj) },
};
STATIC MP_DEFINE_CONST_DICT(extended_blockdev_EBDev_locals_dict, extended_blockdev_EBDev_locals_dict_table);


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
    size_t blksize = mp_obj_int_get_uint_checked(mp_call_method_n_kw(2, 0, self->ioctl));

    // get the size of the device (in number of blocks)
    self->ioctl[2] = MP_OBJ_NEW_SMALL_INT(MP_BLOCKDEV_IOCTL_BLOCK_COUNT); // op
    self->ioctl[3] = MP_OBJ_NEW_SMALL_INT(0); // arg

    // raises TypeError, OverflowError
    size_t blkcnt = mp_obj_int_get_uint_checked(mp_call_method_n_kw(2, 0, self->ioctl));


    self->start_block = 0;
    self->block_count = blkcnt;

    if(n_args >= 2){
        // raises TypeError, OverflowError
        size_t start_bytes = mp_obj_int_get_uint_checked(args[1]);

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
        size_t len_bytes = mp_obj_int_get_uint_checked(args[2]);

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
STATIC void extended_blockdev_EBDev_print(const mp_print_t* print,
                                          mp_obj_t self_in, mp_print_kind_t kind){
    extended_blockdev_EBDev_obj_t* self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "EBDev(start=%d, len=%d)", self->start_block, self->block_count);
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
STATIC mp_obj_t extended_blockdev_EBDev_readblocks(size_t n_args, const mp_obj_t* args){
    extended_blockdev_EBDev_obj_t* self = MP_OBJ_TO_PTR(args[0]);

    if(args[3] != mp_const_none){
        // raises TypeError, OverflowError
        mp_int_t offset = mp_obj_get_int(args[3]);
        if(offset != 0){
            return MP_OBJ_NEW_SMALL_INT(-MP_EINVAL);
        }
    }

    self->readblocks[2] = args[1];
    self->readblocks[3] = args[2];
    mp_obj_t ret = mp_call_method_n_kw(2, 0, self->readblocks);

    if(ret == mp_const_true){
        return MP_OBJ_NEW_SMALL_INT(0);
    }

    return MP_OBJ_NEW_SMALL_INT(-MP_EPERM);
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
STATIC mp_obj_t extended_blockdev_EBDev_writeblocks(size_t n_args, const mp_obj_t* args){
    extended_blockdev_EBDev_obj_t* self = MP_OBJ_TO_PTR(args[0]);

    if(self->writeblocks[0] == MP_OBJ_NULL){
        // read-only block device
        return MP_OBJ_NEW_SMALL_INT(-MP_EROFS);
    }

    if(args[3] != mp_const_none){
        // raises TypeError, OverflowError
        mp_int_t offset = mp_obj_get_int(args[3]);
        if(offset != 0){
            return MP_OBJ_NEW_SMALL_INT(-MP_EINVAL);
        }
    }

    self->writeblocks[2] = args[1];
    self->writeblocks[3] = args[2];
    mp_obj_t ret = mp_call_method_n_kw(2, 0, self->writeblocks);

    if(ret == mp_const_true){
        return MP_OBJ_NEW_SMALL_INT(0);
    }

    return MP_OBJ_NEW_SMALL_INT(-MP_EPERM);
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
STATIC mp_obj_t extended_blockdev_EBDev_ioctl(mp_obj_t self_in, mp_obj_t op_in, mp_obj_t arg_in){
    extended_blockdev_EBDev_obj_t* self = MP_OBJ_TO_PTR(self_in);

    // raises TypeError, OverflowError
    size_t op = mp_obj_int_get_uint_checked(op_in);

    if(op == MP_BLOCKDEV_IOCTL_BLOCK_ERASE){
        return MP_OBJ_NEW_SMALL_INT(0);
    }

    if(op == MP_BLOCKDEV_IOCTL_BLOCK_COUNT){
        return MP_OBJ_NEW_SMALL_INT(self->block_count);
    }

    self->ioctl[2] = op_in;
    self->ioctl[3] = arg_in;
    return mp_call_method_n_kw(2, 0, self->ioctl);
}


STATIC const mp_rom_map_elem_t extended_blockdev_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_extended_blockdev) },

    { MP_ROM_QSTR(MP_QSTR_EBDev), MP_ROM_PTR(&extended_blockdev_EBDev_type) },
};

STATIC MP_DEFINE_CONST_DICT(
    mp_module_extended_blockdev_globals,
    extended_blockdev_globals_table
    );

const mp_obj_module_t mp_module_extended_blockdev = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&mp_module_extended_blockdev_globals,
};

MP_REGISTER_MODULE(MP_QSTR_extended_blockdev, mp_module_extended_blockdev);

#endif /* defined(MODULE_EXTENDED_BLOCKDEV_ENABLED) && MODULE_EXTENDED_BLOCKDEV_ENABLED == 1 */
