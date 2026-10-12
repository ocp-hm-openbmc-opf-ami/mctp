/* SPDX-License-Identifier: GPL-2.0 */
#ifndef MCTP_PID_H
#define MCTP_PID_H

#include <stdint.h>
#include <stdbool.h>
#include <regex.h>
#include <linux/types.h>

#include "mctp.h"

#define PID_SIZE 6
#define MAX_PID_MAP_ENTRIES 64
#define INTERFACE_NAME_LEN 16

/* Individual I3C device configuration */
struct i3c_device {
	char interface[INTERFACE_NAME_LEN + 1];
	uint8_t phys_addr[MAX_ADDR_LEN];
	uint16_t pid_mask;
	char pid_regex_str[64];
	regex_t pid_regex;
	bool pid_regex_valid;
	char name[32];
	int bus_num;
	char role[32];
	bool is_i3c_target;
	bool is_secondary_bus_owner;
	mctp_eid_t local_eid; // Assigned local EID for this device
	mctp_eid_t static_eid; // Static EID from config (0 = not set)
};

#define MAX_I3C_DEVICES 10

/* I3C Bus configuration containing array of devices */
struct i3c_config {
	int num_devices;
	struct i3c_device devices[MAX_I3C_DEVICES];
};

#define MCTP_I3C_PHYS_ADDR_LEN 6

/* Busowner address match types for i3c_is_busowner_addr() */
#define BUSOWNER_ADDR_ANY         0
#define BUSOWNER_ADDR_PRIMARY     1
#define BUSOWNER_ADDR_SECONDARY   2

struct mctp_pid_map_req
{
    mctp_eid_t eid;
    unsigned char pid[PID_SIZE];
};

struct mctp_pid_map_entry_user
{
    mctp_eid_t eid;
    unsigned char pid[6];
};

struct mctp_pid_map_bulk
{
    __u32 count; ///< in/out
    struct mctp_pid_map_entry_user entries[MAX_PID_MAP_ENTRIES];
};

#define SIOCMCTPSETPIDMAP _IOW('m', 1, struct mctp_pid_map_req)
#define SIOCMCTPGETPIDMAP _IOR('m', 3, struct mctp_pid_map_bulk)
#define SIOCMCTPDELPIDMAP _IOW('m', 2, mctp_eid_t)

bool i3c_is_eid_valid(uint8_t eid);
mctp_eid_t pid_lookup_eid_ioctl(const void* pid);
int pid_set_kernel_mapping(mctp_eid_t eid, const void* pid);
int pid_del_kernel_mapping(mctp_eid_t eid);
int pid_display_mappings(void);
mctp_eid_t pid_lookup_eid(struct i3c_config *cfg, const void* pid);
int pid_add_mapping(struct i3c_config *cfg, mctp_eid_t eid, const void* pid);
int pid_del_mapping(struct i3c_config *cfg, mctp_eid_t eid);
int pid_del_all_mappings(struct i3c_config *cfg);
bool i3c_is_busowner_addr(struct i3c_config *config, int type, const uint8_t *hwaddr);
bool i3c_is_primary_busowner_addr(struct i3c_config *config, const uint8_t *hwaddr);
bool i3c_is_secondary_busowner_addr(struct i3c_config *config, const uint8_t *hwaddr);
mctp_eid_t i3c_lookup_static_eid(struct i3c_config *config, const uint8_t *hwaddr);
bool i3c_is_busowner_interface(struct i3c_config *config, const char *ifname);

/* PidMask helpers (regex or numeric) and device lookup */
int i3c_find_by_name(struct i3c_config *config, const char *name);
int i3c_set_pid_mask(struct i3c_device *dev, const char *s);
void i3c_clear_regex(struct i3c_device *dev);

#endif /* MCTP_PID_H */
