/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mctp-pid: PID (Provisioned ID) to EID kernel mapping functions for I3C MCTP
 *
 * Copyright (c) 2021 Code Construct
 * Copyright (c) 2021 Google
 */

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <err.h>

#include "mctp-pid.h"

/**
 * @brief Validate if EID is within valid range
 * @param eid Endpoint ID to validate
 * @return true if EID is valid, false otherwise
 */
bool i3c_is_eid_valid(uint8_t eid)
{
    if (eid < 0x08 || eid == 0xFF)
        return false;
    return true;
}

/**
 * @brief Lookup EID by PID via kernel ioctl (SIOCMCTPGETPIDMAP)
 * @param pid Pointer to 6-byte PID data
 * @return EID if found, 0 if not found or on error
 */
mctp_eid_t pid_lookup_eid_ioctl(const void* pid)
{
	struct mctp_pid_map_bulk bulk = {};
	bulk.count = MAX_PID_MAP_ENTRIES;

	int sock = socket(AF_MCTP, SOCK_DGRAM, 0);
	if (sock < 0)
		return 0;

	if (ioctl(sock, SIOCMCTPGETPIDMAP, &bulk) < 0) {
		close(sock);
		return 0;
	}
	close(sock);

	for (uint32_t i = 0; i < bulk.count; i++) {
		if (memcmp(bulk.entries[i].pid, pid, PID_SIZE) == 0)
			return bulk.entries[i].eid;
	}
	return 0;
}

/**
 * @brief Add PID-to-EID mapping in the kernel
 * @param eid Endpoint ID
 * @param pid Pointer to 6-byte PID data
 * @return 0 on success, -1 on failure
 */
int pid_set_kernel_mapping(mctp_eid_t eid, const void* pid)
{
    struct mctp_pid_map_req req;
    req.eid = eid;
    memcpy(req.pid, pid, PID_SIZE);

    int sock = socket(AF_MCTP, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        warnx("mctp socket error");
        return -1;
    }

    if (ioctl(sock, SIOCMCTPSETPIDMAP, &req) < 0)
    {
        close(sock);
        return -1;
    }
    close(sock);
    return 0;
}

/**
 * @brief Delete PID-to-EID mapping from the kernel
 * @param eid Endpoint ID to delete mapping for
 * @return 0 on success, -1 on failure
 */
int pid_del_kernel_mapping(mctp_eid_t eid)
{
    if (!i3c_is_eid_valid(eid))
    {
		warnx("invalid EID, unable to delete PID Mapping");
        return -1;
    }

    int sock = socket(AF_MCTP, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        warnx("mctp socket error");
        return -1;
    }

    if (ioctl(sock, SIOCMCTPDELPIDMAP, &eid) < 0)
    {
        warnx("ioctl SIOCMCTPDELPIDMAP error");
        close(sock);
        return -1;
    }

    warnx("Mapping deleted: EID %u", eid);

    close(sock);
    return 0;
}

/**
 * @brief Display all PID mappings
 * @return 0 on success, -1 on failure
 */
int pid_display_mappings()
{
    struct mctp_pid_map_bulk bulk = {};
    bulk.count = MAX_PID_MAP_ENTRIES;

    int sock = socket(AF_MCTP, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        warnx("mctp socket error ");
        return -1;
    }

    // Ask kernel to fill in the mapping
    if (ioctl(sock, SIOCMCTPGETPIDMAP, &bulk) < 0)
    {
        warnx("ioctl SIOCMCTPGETPIDMAP error");
        close(sock);
        return -1;
    }

    warnx("Found %u PID ? EID mappings:", bulk.count);
    for (uint32_t i = 0; i < bulk.count; ++i)
    {
        warnx("  EID %3u ? PID %02x:%02x:%02x:%02x:%02x:%02x",
              bulk.entries[i].eid, bulk.entries[i].pid[0],
              bulk.entries[i].pid[1], bulk.entries[i].pid[2],
              bulk.entries[i].pid[3], bulk.entries[i].pid[4],
              bulk.entries[i].pid[5]);
    }

    close(sock);
    return 0;
}

/**
 * @brief Lookup EID by PID using i3c_config devices
 * @param cfg I3C configuration containing device list
 * @param pid Pointer to PID data (6 bytes)
 * @return local_eid if found, 0 if not found
 */
mctp_eid_t pid_lookup_eid(struct i3c_config *cfg, const void* pid)
{
	for (int i = 0; i < cfg->num_devices; i++) {
		struct i3c_device *dev = &cfg->devices[i];
		if (dev->local_eid != 0 &&
		    memcmp(dev->phys_addr, pid, PID_SIZE) == 0) {
			return dev->local_eid;
		}
	}
	return 0;
}

/**
 * @brief Add PID mapping for given EID (kernel + userspace)
 * @param cfg I3C configuration containing device list
 * @param eid Endpoint ID
 * @param pid Pointer to PID data
 * @return 0 on success, -1 on failure
 */
int pid_add_mapping(struct i3c_config *cfg, mctp_eid_t eid, const void* pid)
{
	bool found = false;
	const unsigned char *p = pid;

	// Update local_eid in matching i3c_device
	for (int i = 0; i < cfg->num_devices; i++) {
		struct i3c_device *dev = &cfg->devices[i];
		if (memcmp(dev->phys_addr, pid, PID_SIZE) == 0) {
			dev->local_eid = eid;
			warnx("Updated i3c_device[%d] local_eid=%u PID %02x:%02x:%02x:%02x:%02x:%02x",
			      i, eid, p[0], p[1], p[2], p[3], p[4], p[5]);
			found = true;
			break;
		}
	}

	if (!found) {
		warnx("No matching i3c_device for PID %02x:%02x:%02x:%02x:%02x:%02x",
		      p[0], p[1], p[2], p[3], p[4], p[5]);
	}

	return pid_set_kernel_mapping(eid, pid);
}

/**
 * @brief Delete PID mapping for given EID
 * @param cfg I3C configuration containing device list
 * @param eid Endpoint ID to delete mapping for
 * @return 0 on success, -1 on failure
 */
int pid_del_mapping(struct i3c_config *cfg, mctp_eid_t eid)
{
	// Clear local_eid in matching i3c_device
	for (int i = 0; i < cfg->num_devices; i++) {
		if (cfg->devices[i].local_eid == eid) {
			cfg->devices[i].local_eid = 0;
			break;
		}
	}

	return pid_del_kernel_mapping(eid);
}

/**
 * @brief Delete all PID mappings for cleanup during shutdown
 * @param cfg I3C configuration containing device list
 * @return Number of mappings deleted
 */
int pid_del_all_mappings(struct i3c_config *cfg)
{
	int deleted = 0;

	warnx("Cleaning up all PID mappings during shutdown...");

	for (int i = 0; i < cfg->num_devices; i++) {
		struct i3c_device *dev = &cfg->devices[i];
		if (dev->local_eid != 0) {
			warnx("Deleting PID mapping for EID %u", dev->local_eid);
			pid_del_mapping(cfg, dev->local_eid);
			deleted++;
		}
	}

	warnx("Deleted %d PID mappings during cleanup", deleted);
	return deleted;
}

/**
 * i3c_is_busowner_addr - Check if hardware address matches busowner devices
 * @config: I3C configuration
 * @type: Match type (BUSOWNER_ADDR_ANY, BUSOWNER_ADDR_PRIMARY, or BUSOWNER_ADDR_SECONDARY)
 * @hwaddr: Hardware address to check
 */
bool i3c_is_busowner_addr(struct i3c_config *config, int type, const uint8_t *hwaddr)
{
	if (!config || !hwaddr) {
		return false;
	}

	for (int i = 0; i < config->num_devices; i++) {
		struct i3c_device *dev = &config->devices[i];

		if (strcmp(dev->role, "endpoint") != 0)
			continue;

		if (memcmp(hwaddr, dev->phys_addr, MCTP_I3C_PHYS_ADDR_LEN) != 0)
			continue;

		switch (type) {
		case BUSOWNER_ADDR_ANY:
			return true;
		case BUSOWNER_ADDR_PRIMARY:
			if (!dev->is_secondary_bus_owner)
				return true;
			break;
		case BUSOWNER_ADDR_SECONDARY:
			if (dev->is_secondary_bus_owner)
				return true;
			break;
		default:
			break;
		}
	}
	return false;
}

bool i3c_is_primary_busowner_addr(struct i3c_config *config, const uint8_t *hwaddr)
{
	return i3c_is_busowner_addr(config, BUSOWNER_ADDR_PRIMARY, hwaddr);
}

bool i3c_is_secondary_busowner_addr(struct i3c_config *config, const uint8_t *hwaddr)
{
	return i3c_is_busowner_addr(config, BUSOWNER_ADDR_SECONDARY, hwaddr);
}

/**
 * i3c_lookup_static_eid - Look up static EID for an I3C device by hardware address
 * @config: I3C configuration
 * @hwaddr: 6-byte hardware address (PID) to match
 *
 * Returns static_eid if found and non-zero, otherwise 0.
 */
mctp_eid_t i3c_lookup_static_eid(struct i3c_config *config,
				 const uint8_t *hwaddr)
{
	if (!config || !hwaddr)
		return 0;

	for (int i = 0; i < config->num_devices; i++) {
		struct i3c_device *dev = &config->devices[i];
		if (memcmp(dev->phys_addr, hwaddr, MCTP_I3C_PHYS_ADDR_LEN) == 0)
			return dev->static_eid;
	}
	return 0;
}

/**
 * i3c_is_busowner_interface - Check if interface name matches any I3C busowner device
 * @config: I3C configuration
 * @ifname: Interface name to check
 */
bool i3c_is_busowner_interface(struct i3c_config *config, const char *ifname)
{
	if (!config || !ifname)
		return false;

	for (int i = 0; i < config->num_devices; i++) {
		struct i3c_device *dev = &config->devices[i];

		if (strcmp(dev->role, "endpoint") != 0)
			continue;

		if (strcmp(dev->interface, ifname) == 0)
			return true;
	}
	return false;
}

/**
 * i3c_find_by_name - Look up an i3c_device by name
 * @config: I3C configuration
 * @name: device name to match
 *
 * Returns the index in @config->devices, or -1 if not found.
 */
int i3c_find_by_name(struct i3c_config *config, const char *name)
{
	if (!config || !name)
		return -1;

	for (int i = 0; i < config->num_devices; i++) {
		if (strncmp(config->devices[i].name, name,
			    sizeof(config->devices[i].name)) == 0)
			return i;
	}
	return -1;
}

/**
 * i3c_set_pid_mask - Parse a PidMask string as either a regex or hex integer
 * @dev: device to populate
 * @s: PidMask string from JSON/TOML
 *
 * Numeric forms (e.g. "0x20a", "1234") populate dev->pid_mask only.
 * Anything containing regex metacharacters is compiled into dev->pid_regex.
 *
 * Returns 0 on success, -1 on regex compile error.
 */
int i3c_set_pid_mask(struct i3c_device *dev, const char *s)
{
	if (!dev || !s || !*s)
		return 0;

	/* Detect regex metacharacters */
	bool is_regex = false;
	for (const char *p = s; *p; p++) {
		if (strchr(".[](){}|*+?^$\\", *p)) {
			is_regex = true;
			break;
		}
	}

	if (is_regex) {
		strncpy(dev->pid_regex_str, s, sizeof(dev->pid_regex_str) - 1);
		dev->pid_regex_str[sizeof(dev->pid_regex_str) - 1] = '\0';
		int rc = regcomp(&dev->pid_regex, dev->pid_regex_str,
				 REG_EXTENDED | REG_ICASE | REG_NOSUB);
		if (rc != 0) {
			char errbuf[128];
			regerror(rc, &dev->pid_regex, errbuf, sizeof(errbuf));
			warnx("Invalid PidMask regex '%s': %s",
			      dev->pid_regex_str, errbuf);
			dev->pid_regex_str[0] = '\0';
			return -1;
		}
		dev->pid_regex_valid = true;
		return 0;
	}

	/* Numeric form */
	dev->pid_mask = (uint16_t)strtoul(s, NULL, 0);
	return 0;
}

/**
 * i3c_clear_regex - Free a compiled PidMask regex if present
 * @dev: device whose regex should be released
 */
void i3c_clear_regex(struct i3c_device *dev)
{
	if (!dev)
		return;
	if (dev->pid_regex_valid) {
		regfree(&dev->pid_regex);
		dev->pid_regex_valid = false;
		dev->pid_regex_str[0] = '\0';
	}
}
