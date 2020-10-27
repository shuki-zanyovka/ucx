/**
* Copyright (C) NVIDIA Corporation. 2019.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "topo.h"
#include "string.h"

#include <ucs/datastruct/khash.h>
#include <ucs/type/spinlock.h>
#include <ucs/debug/assert.h>
#include <ucs/debug/log.h>
#include <inttypes.h>
#include <float.h>


#define UCS_TOPO_MAX_SYS_DEVICES  256
#define UCS_TOPO_SYSFS_PCI_PREFIX "/sys/class/pci_bus"

typedef int64_t ucs_bus_id_bit_rep_t;

typedef struct ucs_topo_sys_dev_to_bus_arr {
    ucs_sys_bus_id_t bus_arr[UCS_TOPO_MAX_SYS_DEVICES];
    unsigned         count;
} ucs_topo_sys_dev_to_bus_arr_t;

KHASH_MAP_INIT_INT64(bus_to_sys_dev, ucs_sys_device_t);

typedef struct ucs_topo_global_ctx {
    khash_t(bus_to_sys_dev)       bus_to_sys_dev_hash;
    ucs_spinlock_t                lock;
    ucs_topo_sys_dev_to_bus_arr_t sys_dev_to_bus_lookup;
} ucs_topo_global_ctx_t;


static ucs_topo_global_ctx_t ucs_topo_ctx;

static ucs_bus_id_bit_rep_t ucs_topo_get_bus_id_bit_repr(const ucs_sys_bus_id_t *bus_id)
{
    return (((uint64_t)bus_id->domain << 24) |
            ((uint64_t)bus_id->bus << 16)    |
            ((uint64_t)bus_id->slot << 8)    |
            (bus_id->function));
}

void ucs_topo_init()
{
    ucs_spinlock_init(&ucs_topo_ctx.lock, 0);
    kh_init_inplace(bus_to_sys_dev, &ucs_topo_ctx.bus_to_sys_dev_hash);
    ucs_topo_ctx.sys_dev_to_bus_lookup.count = 0;
}

void ucs_topo_cleanup()
{
    kh_destroy_inplace(bus_to_sys_dev, &ucs_topo_ctx.bus_to_sys_dev_hash);
    ucs_spinlock_destroy(&ucs_topo_ctx.lock);
}

ucs_status_t ucs_topo_find_device_by_bus_id(const ucs_sys_bus_id_t *bus_id,
                                            ucs_sys_device_t *sys_dev)
{
    khiter_t hash_it;
    ucs_kh_put_t kh_put_status;
    ucs_bus_id_bit_rep_t bus_id_bit_rep;

    bus_id_bit_rep  = ucs_topo_get_bus_id_bit_repr(bus_id);

    ucs_spin_lock(&ucs_topo_ctx.lock);
    hash_it = kh_put(bus_to_sys_dev /*name*/,
                     &ucs_topo_ctx.bus_to_sys_dev_hash /*pointer to hashmap*/,
                     bus_id_bit_rep /*key*/,
                     &kh_put_status);

    if (kh_put_status == UCS_KH_PUT_KEY_PRESENT) {
        *sys_dev = kh_value(&ucs_topo_ctx.bus_to_sys_dev_hash, hash_it);
        ucs_debug("bus id 0x%"PRIx64" exists. sys_dev = %u", bus_id_bit_rep,
                  *sys_dev);
    } else if ((kh_put_status == UCS_KH_PUT_BUCKET_EMPTY) ||
               (kh_put_status == UCS_KH_PUT_BUCKET_CLEAR)) {
        ucs_assert_always(ucs_topo_ctx.sys_dev_to_bus_lookup.count <
                          UCS_TOPO_MAX_SYS_DEVICES);
        *sys_dev = ucs_topo_ctx.sys_dev_to_bus_lookup.count;
        kh_value(&ucs_topo_ctx.bus_to_sys_dev_hash, hash_it) = *sys_dev;
        ucs_debug("bus id 0x%"PRIx64" doesn't exist. sys_dev = %u",
                  bus_id_bit_rep, *sys_dev);

        ucs_topo_ctx.sys_dev_to_bus_lookup.bus_arr[*sys_dev] = *bus_id;
        ucs_topo_ctx.sys_dev_to_bus_lookup.count++;
    }

    ucs_spin_unlock(&ucs_topo_ctx.lock);
    return UCS_OK;
}

static void
ucs_topo_get_bus_path(const ucs_sys_bus_id_t *bus_id, char *path, size_t max)
{
    ucs_snprintf_safe(path, max, "%s/%04x:%02x", UCS_TOPO_SYSFS_PCI_PREFIX,
                      bus_id->domain, bus_id->bus);
}

ucs_status_t ucs_topo_get_distance(ucs_sys_device_t device1,
                                   ucs_sys_device_t device2,
                                   ucs_sys_dev_distance_t *distance)
{
    char path1[PATH_MAX], path2[PATH_MAX];
    ssize_t path_distance;

    /* If one of the devices is unknown, we assume near topology */
    if ((device1 == UCS_SYS_DEVICE_ID_UNKNOWN) ||
        (device2 == UCS_SYS_DEVICE_ID_UNKNOWN) || (device1 == device2)) {
        path_distance = 0;
    } else {
        if ((device1 >= ucs_topo_ctx.sys_dev_to_bus_lookup.count) ||
            (device2 >= ucs_topo_ctx.sys_dev_to_bus_lookup.count)) {
            return UCS_ERR_INVALID_PARAM;
        }

        ucs_topo_get_bus_path(&ucs_topo_ctx.sys_dev_to_bus_lookup.bus_arr[device1],
                              path1, sizeof(path1));
        ucs_topo_get_bus_path(&ucs_topo_ctx.sys_dev_to_bus_lookup.bus_arr[device2],
                              path2, sizeof(path2));

        path_distance = ucs_path_calc_distance(path1, path2);
        if (path_distance < 0) {
            return (ucs_status_t)path_distance;
        }
    }

    /* Rough approximation of bandwidth/latency as function of PCI distance in
     * sysfs.
     * TODO implement more accurate estimation, based on system type, PCIe
     * switch, etc.
     */
    if (path_distance <= 2) {
        distance->latency   = 0;
        distance->bandwidth = DBL_MAX;
    } else if (path_distance <= 4) {
        distance->latency   = 300e-9;
        distance->bandwidth = 2000 * UCS_MBYTE;
    } else {
        distance->latency   = 900e-9;
        distance->bandwidth = 300 * UCS_MBYTE;
    }

    return UCS_OK;
}

const char *
ucs_topo_sys_device_bdf_name(ucs_sys_device_t sys_dev, char *buffer, size_t max)
{
    const ucs_sys_bus_id_t* bus_id;

    if (sys_dev == UCS_SYS_DEVICE_ID_UNKNOWN) {
        return "<unknown>";
    }

    if (sys_dev >= ucs_topo_ctx.sys_dev_to_bus_lookup.count) {
        return NULL;
    }

    bus_id = &ucs_topo_ctx.sys_dev_to_bus_lookup.bus_arr[sys_dev];
    ucs_snprintf_safe(buffer, max, "%04x:%02x:%02x.%d", bus_id->domain,
                      bus_id->bus, bus_id->slot, bus_id->function);
    return buffer;
}

void ucs_topo_print_info(FILE *stream)
{
}
