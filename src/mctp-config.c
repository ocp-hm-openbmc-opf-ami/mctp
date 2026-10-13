/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mctp-config.c - MCTP I3C/PCIe configuration loaders
 *
 * Implements EntityManager (D-Bus) and JSON (file) loaders that populate
 * a struct i3c_config. PCIe role updates are forwarded via a caller-
 * supplied callback so this translation unit does not depend on the
 * mctpd struct ctx.
 */

#include <err.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>

#include <systemd/sd-bus.h>
#include <json-c/json.h>

#include "mctp-config.h"
#include "mctp-pid.h"

/* ---- EntityManager loader ----------------------------------------------- */

static const char *EM_SERVICE  = "xyz.openbmc_project.EntityManager";
static const char *EM_PATH     = "/xyz/openbmc_project/inventory";
static const char *EM_OM_IFACE = "org.freedesktop.DBus.ObjectManager";
static const char *EM_CFG_PFX  = "xyz.openbmc_project.Configuration.";

/* Read a "v" containing a string. */
static int em_read_v_str(sd_bus_message *m, const char **out)
{
	return sd_bus_message_read(m, "v", "s", out);
}

/* Read a "v" containing any integer-like type, return as int64. */
static int em_read_v_i64(sd_bus_message *m, int64_t *out)
{
	char type;
	const char *contents;
	int r = sd_bus_message_peek_type(m, &type, &contents);
	if (r < 0) return r;
	if (type != 'v' || !contents || !*contents) return -EINVAL;
	int rr = 0;
	int64_t v = 0;
	switch (contents[0]) {
	case 'y': { uint8_t  x; rr = sd_bus_message_read(m, "v", "y", &x); v = x; break; }
	case 'b': { int      x; rr = sd_bus_message_read(m, "v", "b", &x); v = x; break; }
	case 'n': { int16_t  x; rr = sd_bus_message_read(m, "v", "n", &x); v = x; break; }
	case 'q': { uint16_t x; rr = sd_bus_message_read(m, "v", "q", &x); v = x; break; }
	case 'i': { int32_t  x; rr = sd_bus_message_read(m, "v", "i", &x); v = x; break; }
	case 'u': { uint32_t x; rr = sd_bus_message_read(m, "v", "u", &x); v = x; break; }
	case 'x': { int64_t  x; rr = sd_bus_message_read(m, "v", "x", &x); v = x; break; }
	case 't': { uint64_t x; rr = sd_bus_message_read(m, "v", "t", &x); v = (int64_t)x; break; }
	case 'd': { double   x; rr = sd_bus_message_read(m, "v", "d", &x); v = (int64_t)x; break; }
	default: return -EINVAL;
	}
	if (rr < 0) return rr;
	*out = v;
	return 0;
}

static int em_read_v_bool(sd_bus_message *m, bool *out)
{
	int64_t v;
	int r = em_read_v_i64(m, &v);
	if (r < 0) return r;
	*out = (v != 0);
	return 0;
}

/* Read a "v" containing an array of integers, store first @cap into @buf. */
static int em_read_v_byte_array(sd_bus_message *m, uint8_t *buf,
				size_t cap, size_t *outlen)
{
	char type;
	const char *contents;
	int r = sd_bus_message_peek_type(m, &type, &contents);
	if (r < 0) return r;
	if (type != 'v' || !contents || contents[0] != 'a' || !contents[1])
		return -EINVAL;
	char elem = contents[1];

	r = sd_bus_message_enter_container(m, 'v', contents);
	if (r < 0) return r;
	char inner_sig[2] = { elem, 0 };
	r = sd_bus_message_enter_container(m, 'a', inner_sig);
	if (r < 0) {
		sd_bus_message_exit_container(m);
		return r;
	}

	size_t n = 0;
	for (;;) {
		int rr = 0;
		int64_t v = 0;
		switch (elem) {
		case 'y': { uint8_t  x; rr = sd_bus_message_read_basic(m, 'y', &x); v = x; break; }
		case 'n': { int16_t  x; rr = sd_bus_message_read_basic(m, 'n', &x); v = x; break; }
		case 'q': { uint16_t x; rr = sd_bus_message_read_basic(m, 'q', &x); v = x; break; }
		case 'i': { int32_t  x; rr = sd_bus_message_read_basic(m, 'i', &x); v = x; break; }
		case 'u': { uint32_t x; rr = sd_bus_message_read_basic(m, 'u', &x); v = x; break; }
		case 'x': { int64_t  x; rr = sd_bus_message_read_basic(m, 'x', &x); v = x; break; }
		case 't': { uint64_t x; rr = sd_bus_message_read_basic(m, 't', &x); v = (int64_t)x; break; }
		default: rr = -EINVAL; break;
		}
		if (rr <= 0) break;
		if (n < cap) buf[n] = (uint8_t)v;
		n++;
	}
	sd_bus_message_exit_container(m);
	sd_bus_message_exit_container(m);
	if (outlen) *outlen = n;
	return 0;
}

/* Apply parsed properties for one MCTPI3CTarget interface. */
static int em_apply_i3c_target(struct i3c_config *cfg,
			       const char *name, int has_bus, int64_t bus_num,
			       const char *role, int has_target,
			       bool i3c_target_val,
			       int has_pid_mask, const char *pid_mask_str,
			       int has_sec_bo, bool sec_bo,
			       int has_addr, const uint8_t *addr,
			       size_t addr_len,
			       int has_static_eid, const char *static_eid_str,
			       const char *phys_link_name)
{
	int slot = (name && *name)
			   ? i3c_find_by_name(cfg, name) : -1;
	if (slot < 0) {
		if (cfg->num_devices >= MAX_I3C_DEVICES) {
			warnx("EM: too many I3C devices, skipping %s",
			      (name && *name) ? name : "(unnamed)");
			return -1;
		}
		slot = cfg->num_devices;
	}

	struct i3c_device *dev = &cfg->devices[slot];
	i3c_clear_regex(dev);
	memset(dev, 0, sizeof(*dev));

	if (name && *name)
		strncpy(dev->name, name, sizeof(dev->name) - 1);
	if (has_bus)
		dev->bus_num = (int)bus_num;

	const char *r = (role && *role) ? role : "endpoint";
	strncpy(dev->role, r, sizeof(dev->role) - 1);

	if (has_target)
		dev->is_i3c_target = i3c_target_val;
	else
		dev->is_i3c_target = (strcmp(dev->role, "bus-owner") != 0);

	if (has_pid_mask && pid_mask_str)
		i3c_set_pid_mask(dev, pid_mask_str);

	if (has_sec_bo)
		dev->is_secondary_bus_owner = sec_bo;

	if (has_addr) {
		size_t cap = sizeof(dev->phys_addr);
		size_t n = addr_len < cap ? addr_len : cap;
		memcpy(dev->phys_addr, addr, n);
	}

	if (has_static_eid && static_eid_str)
		dev->static_eid = (mctp_eid_t)strtoul(static_eid_str, NULL, 0);

	if (phys_link_name && *phys_link_name)
		strncpy(dev->interface, phys_link_name,
			sizeof(dev->interface) - 1);

	/* Only set interface from bus_num if not already set
	 * (e.g. by MctpConfig PhysicalLinkName or MCTPI3CTarget PhysicalLinkName).
	 * The netdev name is authoritative when declared via PhysicalLinkName; the
	 * bus-number form is a SoC-agnostic fallback used only when it is omitted. */
	if (!dev->interface[0]) {
		if (!dev->is_i3c_target) {
			snprintf(dev->interface, INTERFACE_NAME_LEN,
				 "mctpi3c%d", dev->bus_num);
		} else {
			snprintf(dev->interface, INTERFACE_NAME_LEN,
				 "mctpi3c-target%d", dev->bus_num);
		}
	}

	warnx("EM I3C device[%d]: name=%s bus=%d role=%s is_i3c_target=%d iface=%s pid_mask=0x%04X regex='%s'",
	      slot, dev->name, dev->bus_num, dev->role,
	      dev->is_i3c_target, dev->interface, dev->pid_mask,
	      dev->pid_regex_valid ? dev->pid_regex_str : "");

	if (slot == cfg->num_devices)
		cfg->num_devices++;
	return 0;
}

/* Parse properties of one MCTPI3CTarget interface. */
static int em_parse_i3c_target(struct i3c_config *cfg, sd_bus_message *m)
{
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0) return r;

	char name[64] = {0};
	char role[32] = {0};
	int64_t bus_num = 0;       int has_bus = 0;
	int has_target = 0;        bool i3c_target_val = true;
	int has_pid_mask = 0;      char pid_mask_str[64] = {0};
	int has_sec_bo = 0;        bool sec_bo = false;
	int has_addr = 0;          uint8_t addr[MCTP_I3C_PHYS_ADDR_LEN] = {0};
	size_t addr_len = 0;
	int has_static_eid = 0;    char static_eid_str[16] = {0};
	char phys_link_name[INTERFACE_NAME_LEN] = {0};

	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key = NULL;
		r = sd_bus_message_read(m, "s", &key);
		if (r < 0) goto exit_entry;

		if (strcmp(key, "Name") == 0) {
			const char *s = NULL;
			if (em_read_v_str(m, &s) >= 0 && s)
				strncpy(name, s, sizeof(name) - 1);
		} else if (strcmp(key, "Bus") == 0) {
			if (em_read_v_i64(m, &bus_num) >= 0)
				has_bus = 1;
		} else if (strcmp(key, "Role") == 0) {
			const char *s = NULL;
			if (em_read_v_str(m, &s) >= 0 && s)
				strncpy(role, s, sizeof(role) - 1);
		} else if (strcmp(key, "I3CTarget") == 0) {
			if (em_read_v_bool(m, &i3c_target_val) >= 0)
				has_target = 1;
		} else if (strcmp(key, "PidMask") == 0) {
			const char *s = NULL;
			if (em_read_v_str(m, &s) >= 0 && s) {
				strncpy(pid_mask_str, s,
					sizeof(pid_mask_str) - 1);
				has_pid_mask = 1;
			}
		} else if (strcmp(key, "SecondaryBusOwner") == 0) {
			if (em_read_v_bool(m, &sec_bo) >= 0)
				has_sec_bo = 1;
		} else if (strcmp(key, "Address") == 0) {
			if (em_read_v_byte_array(m, addr, sizeof(addr),
						 &addr_len) >= 0)
				has_addr = 1;
		} else if (strcmp(key, "StaticEndpointID") == 0) {
			/* EM exposes StaticEndpointID as uint64 (t), not a
			 * string.  Try the integer path first; fall back to
			 * string for legacy compatibility.  If both fail,
			 * skip the variant so the message cursor stays
			 * consistent. */
			int64_t eid_val = 0;
			if (em_read_v_i64(m, &eid_val) >= 0) {
				snprintf(static_eid_str, sizeof(static_eid_str),
					 "%lld", (long long)eid_val);
				has_static_eid = 1;
			} else {
				const char *s = NULL;
				if (em_read_v_str(m, &s) >= 0 && s) {
					strncpy(static_eid_str, s,
						sizeof(static_eid_str) - 1);
					has_static_eid = 1;
				} else {
					sd_bus_message_skip(m, "v");
				}
			}
		} else if (strcmp(key, "PhysicalLinkName") == 0) {
			const char *s = NULL;
			if (em_read_v_str(m, &s) >= 0 && s)
				strncpy(phys_link_name, s,
					sizeof(phys_link_name) - 1);
		} else {
			sd_bus_message_skip(m, "v");
		}
exit_entry:
		sd_bus_message_exit_container(m);
		if (r < 0) break;
	}
	sd_bus_message_exit_container(m);

	em_apply_i3c_target(cfg, name, has_bus, bus_num,
			    role, has_target, i3c_target_val,
			    has_pid_mask, pid_mask_str,
			    has_sec_bo, sec_bo,
			    has_addr, addr, addr_len,
			    has_static_eid, static_eid_str,
			    phys_link_name);
	return 1;
}

/*
 * Parse properties of one MctpConfig interface.
 *
 * MctpConfig carries Role, PhysicalLinkName, PhysicalLinkType, NetworkID,
 * MTU etc.  When PhysicalLinkType == "i3c", we create (or update) an
 * i3c_device using PhysicalLinkName as the interface name directly, instead
 * of generating it from a bus number.  When PhysicalLinkType == "pcie",
 * we populate pcie_cfg so the link gets the correct endpoint/bus-owner role.
 */
static int em_parse_mctp_config(struct i3c_config *cfg, sd_bus_message *m,
				struct pcie_config *pcie_cfg,
				mctp_set_pcie_role_fn pcie_cb, void *user)
{
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0) return r;

	char name[64] = {0};
	char role[32] = {0};
	char phys_link_name[INTERFACE_NAME_LEN + 1] = {0};
	char phys_link_type[32] = {0};

	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key = NULL;
		r = sd_bus_message_read(m, "s", &key);
		if (r < 0) {
			sd_bus_message_exit_container(m);
			break;
		}
		if (strcmp(key, "Name") == 0) {
			const char *s = NULL;
			if (em_read_v_str(m, &s) >= 0 && s)
				strncpy(name, s, sizeof(name) - 1);
		} else if (strcmp(key, "Role") == 0) {
			const char *s = NULL;
			if (em_read_v_str(m, &s) >= 0 && s)
				strncpy(role, s, sizeof(role) - 1);
		} else if (strcmp(key, "PhysicalLinkName") == 0) {
			const char *s = NULL;
			if (em_read_v_str(m, &s) >= 0 && s)
				strncpy(phys_link_name, s,
					sizeof(phys_link_name) - 1);
		} else if (strcmp(key, "PhysicalLinkType") == 0) {
			const char *s = NULL;
			if (em_read_v_str(m, &s) >= 0 && s)
				strncpy(phys_link_type, s,
					sizeof(phys_link_type) - 1);
		} else {
			sd_bus_message_skip(m, "v");
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);

	/* Handle PCIe link type: populate pcie_cfg with the role. */
	if (strcasecmp(phys_link_type, "pcie") == 0) {
		if (!pcie_cfg || pcie_cfg->num_interfaces >= MAX_PCIE_INTERFACES) {
			warnx("EM MctpConfig '%s': too many PCIe interfaces, skipping",
			      name);
			return 0;
		}
		if (pcie_cb && role[0] && pcie_cb(user, role) != 0)
			warnx("EM MctpConfig: invalid PCIe role '%s'", role);
		struct pcie_interface_config *iface =
			&pcie_cfg->interfaces[pcie_cfg->num_interfaces++];
		strncpy(iface->name,      name,           sizeof(iface->name) - 1);
		strncpy(iface->interface, phys_link_name, sizeof(iface->interface) - 1);
		strncpy(iface->role,      role,           sizeof(iface->role) - 1);
		iface->enabled = true;
		warnx("EM MctpConfig PCIe iface[%d]: name=%s link=%s role=%s",
		      pcie_cfg->num_interfaces - 1,
		      iface->name, iface->interface, iface->role);
		return 0;
	}

	/* Only handle I3C link types beyond this point. */
	if (strcasecmp(phys_link_type, "i3c") != 0) {
		warnx("EM MctpConfig '%s': PhysicalLinkType='%s', skipping (not i3c)",
		      name, phys_link_type);
		return 0;
	}

	int slot = (name[0]) ? i3c_find_by_name(cfg, name) : -1;
	if (slot < 0) {
		if (cfg->num_devices >= MAX_I3C_DEVICES) {
			warnx("EM MctpConfig: too many I3C devices, skipping %s",
			      name[0] ? name : "(unnamed)");
			return -1;
		}
		slot = cfg->num_devices;
		memset(&cfg->devices[slot], 0, sizeof(cfg->devices[slot]));
	}

	struct i3c_device *dev = &cfg->devices[slot];

	if (name[0])
		strncpy(dev->name, name, sizeof(dev->name) - 1);

	const char *r_str = (role[0]) ? role : "endpoint";
	strncpy(dev->role, r_str, sizeof(dev->role) - 1);

	/* Accept both "bus-owner" and "busowner" (Entity Manager variants). */
	dev->is_i3c_target = (strcmp(dev->role, "bus-owner") != 0 &&
			      strcasecmp(dev->role, "busowner") != 0 &&
			      strcasecmp(dev->role, "bus_owner") != 0);

	/* Use PhysicalLinkName directly as interface name. */
	if (phys_link_name[0])
		strncpy(dev->interface, phys_link_name,
			sizeof(dev->interface) - 1);

	warnx("EM MctpConfig device[%d]: name=%s role=%s is_i3c_target=%d iface=%s",
	      slot, dev->name, dev->role, dev->is_i3c_target, dev->interface);

	if (slot == cfg->num_devices)
		cfg->num_devices++;
	return 1;
}

/*
 * Parse properties of one MctpI3CConfig interface.
 *
 * MctpI3CConfig carries BusNumber and PIDMask.  We look up an existing
 * device by Name (created earlier by em_parse_mctp_config) and fill in
 * the bus/pid fields.  If the device doesn't exist yet we create it
 * (MctpI3CConfig may be iterated before MctpConfig).
 */
static int em_parse_mctp_i3c_config(struct i3c_config *cfg, sd_bus_message *m)
{
	int r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0) return r;

	char name[64] = {0};
	int64_t bus_num = 0;     int has_bus = 0;
	char pid_mask_str[64] = {0};  int has_pid = 0;

	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key = NULL;
		r = sd_bus_message_read(m, "s", &key);
		if (r < 0) {
			sd_bus_message_exit_container(m);
			break;
		}
		if (strcmp(key, "Name") == 0) {
			const char *s = NULL;
			if (em_read_v_str(m, &s) >= 0 && s)
				strncpy(name, s, sizeof(name) - 1);
		} else if (strcmp(key, "BusNumber") == 0) {
			if (em_read_v_i64(m, &bus_num) >= 0)
				has_bus = 1;
		} else if (strcmp(key, "PIDMask") == 0 ||
			   strcmp(key, "PidMask") == 0) {
			const char *s = NULL;
			if (em_read_v_str(m, &s) >= 0 && s) {
				strncpy(pid_mask_str, s,
					sizeof(pid_mask_str) - 1);
				has_pid = 1;
			}
		} else {
			sd_bus_message_skip(m, "v");
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);

	int slot = (name[0]) ? i3c_find_by_name(cfg, name) : -1;
	if (slot < 0) {
		/* Device may not exist yet if MctpConfig hasn't been parsed. */
		if (cfg->num_devices >= MAX_I3C_DEVICES) {
			warnx("EM MctpI3CConfig: too many I3C devices, skipping %s",
			      name[0] ? name : "(unnamed)");
			return -1;
		}
		slot = cfg->num_devices;
		memset(&cfg->devices[slot], 0, sizeof(cfg->devices[slot]));
		if (name[0])
			strncpy(cfg->devices[slot].name, name,
				sizeof(cfg->devices[slot].name) - 1);
		cfg->num_devices++;
	}

	struct i3c_device *dev = &cfg->devices[slot];

	if (has_bus)
		dev->bus_num = (int)bus_num;
	if (has_pid) {
		i3c_clear_regex(dev);
		i3c_set_pid_mask(dev, pid_mask_str);
	}

	/* If interface was not set by MctpConfig, generate from bus_num. */
	if (!dev->interface[0] && has_bus) {
		snprintf(dev->interface, INTERFACE_NAME_LEN,
			 "mctpi3c%d", dev->bus_num);
	}

	warnx("EM MctpI3CConfig device[%d]: name=%s bus=%d iface=%s pid_mask=0x%04X regex='%s'",
	      slot, dev->name, dev->bus_num, dev->interface, dev->pid_mask,
	      dev->pid_regex_valid ? dev->pid_regex_str : "");

	return 0;
}

/* Parse properties of one MCTPPCIeConfiguration interface and append an
 * entry to @pcie_cfg.  The @pcie_cb callback is still invoked for the
 * Role property so the caller can update its global role fallback.
 */
static int em_parse_pcie_config(sd_bus_message *m,
				struct pcie_config *pcie_cfg,
				mctp_set_pcie_role_fn pcie_cb, void *user)
{
	char name[32] = {0};
	char link[INTERFACE_NAME_LEN + 1] = {0};
	char role[32] = {0};
	bool enabled = true; /* default: enabled */

	int r = sd_bus_message_enter_container(m, 'a', "{sv}");
	if (r < 0) return r;

	while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
		const char *key = NULL;
		r = sd_bus_message_read(m, "s", &key);
		if (r < 0) {
			sd_bus_message_exit_container(m);
			break;
		}
		if (strcmp(key, "Name") == 0) {
			const char *s = NULL;
			if (em_read_v_str(m, &s) >= 0 && s)
				strncpy(name, s, sizeof(name) - 1);
		} else if (strcmp(key, "Enabled") == 0) {
			const char *s = NULL;
			if (em_read_v_str(m, &s) >= 0 && s)
				enabled = (strcmp(s, "enabled") == 0);
		} else if (strcmp(key, "PhysicalLinkName") == 0) {
			const char *s = NULL;
			if (em_read_v_str(m, &s) >= 0 && s)
				strncpy(link, s, sizeof(link) - 1);
		} else if (strcmp(key, "Role") == 0) {
			const char *s = NULL;
			if (em_read_v_str(m, &s) >= 0 && s) {
				strncpy(role, s, sizeof(role) - 1);
				if (pcie_cb && pcie_cb(user, s) != 0)
					warnx("EM: invalid PCIe role '%s'", s);
			}
		} else if (strcmp(key, "Net") == 0) {
			sd_bus_message_skip(m, "v");
		} else if (strcmp(key, "OwnEID") == 0) {
			sd_bus_message_skip(m, "v");
		} else if (strcmp(key, "MTU") == 0) {
			sd_bus_message_skip(m, "v");
		} else {
			sd_bus_message_skip(m, "v");
		}
		sd_bus_message_exit_container(m);
	}
	sd_bus_message_exit_container(m);

	if (!pcie_cfg)
		return 0;

	if (pcie_cfg->num_interfaces >= MAX_PCIE_INTERFACES) {
		warnx("EM: too many MCTPPCIeConfiguration entries, skipping '%s'", name);
		return 0;
	}

	struct pcie_interface_config *iface =
		&pcie_cfg->interfaces[pcie_cfg->num_interfaces++];
	strncpy(iface->name,      name, sizeof(iface->name) - 1);
	strncpy(iface->interface, link, sizeof(iface->interface) - 1);
	strncpy(iface->role,      role, sizeof(iface->role) - 1);
	iface->enabled = enabled;

	warnx("EM PCIe iface[%d]: name=%s link=%s role=%s enabled=%d",
	      pcie_cfg->num_interfaces - 1,
	      iface->name, iface->interface, iface->role,
	      iface->enabled);
	return 0;
}

int mctp_load_from_entity_manager(struct i3c_config *cfg,
				  struct pcie_config *pcie_cfg,
				  sd_bus *bus_in,
				  mctp_set_pcie_role_fn pcie_cb, void *user)
{
	sd_bus *bus = NULL;
	sd_bus_message *reply = NULL;
	sd_bus_error err = SD_BUS_ERROR_NULL;
	int r, count = 0;
	bool owned_bus = false;

	if (!cfg)
		return -1;

	if (bus_in) {
		bus = sd_bus_ref(bus_in);
	} else {
		r = sd_bus_open_system(&bus);
		if (r < 0) {
			warnx("EM: cannot open system bus: %s", strerror(-r));
			return -1;
		}
		owned_bus = true;
	}

	r = sd_bus_call_method(bus, EM_SERVICE, EM_PATH, EM_OM_IFACE,
			       "GetManagedObjects", &err, &reply, "");
	if (r < 0) {
		warnx("EM GetManagedObjects: %s",
		      err.message ? err.message : strerror(-r));
		count = -1;
		goto cleanup;
	}

	r = sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}");
	if (r < 0) { count = -1; goto cleanup; }

	while ((r = sd_bus_message_enter_container(
			reply, 'e', "oa{sa{sv}}")) > 0) {
		const char *path = NULL;
		r = sd_bus_message_read(reply, "o", &path);
		if (r < 0) {
			sd_bus_message_exit_container(reply);
			break;
		}
		r = sd_bus_message_enter_container(reply, 'a', "{sa{sv}}");
		if (r < 0) {
			sd_bus_message_exit_container(reply);
			break;
		}
		while ((r = sd_bus_message_enter_container(
				reply, 'e', "sa{sv}")) > 0) {
			const char *iface = NULL;
			r = sd_bus_message_read(reply, "s", &iface);
			if (r < 0) {
				sd_bus_message_exit_container(reply);
				break;
			}
			size_t pfx = strlen(EM_CFG_PFX);
			if (strncmp(iface, EM_CFG_PFX, pfx) == 0) {
				const char *type = iface + pfx;
				if (strcmp(type, "MCTPI3CTarget") == 0) {
					if (em_parse_i3c_target(cfg, reply) > 0)
						count++;
				} else if (strcmp(type, "MCTPPCIeConfiguration") == 0) {
					em_parse_pcie_config(reply, pcie_cfg, pcie_cb, user);
				} else if (strcmp(type, "MctpConfig") == 0) {
					if (em_parse_mctp_config(cfg, reply, pcie_cfg, pcie_cb, user) > 0)
						count++;
				} else if (strcmp(type, "MctpI3CConfig") == 0) {
					em_parse_mctp_i3c_config(cfg, reply);
				} else {
					sd_bus_message_skip(reply, "a{sv}");
				}
			} else {
				sd_bus_message_skip(reply, "a{sv}");
			}
			sd_bus_message_exit_container(reply);
		}
		sd_bus_message_exit_container(reply); /* a{sa{sv}} */
		sd_bus_message_exit_container(reply); /* dict entry */
	}
	sd_bus_message_exit_container(reply);

	warnx("EM config loaded: %d MCTPI3CTarget device(s)", count);

cleanup:
	sd_bus_error_free(&err);
	sd_bus_message_unref(reply);
	sd_bus_unref(bus);
	(void)owned_bus;
	return count;
}

/* ---- JSON file loader --------------------------------------------------- */

int mctp_parse_json_file(struct i3c_config *cfg,
			 struct pcie_config *pcie_cfg,
			 const char *filename,
			 mctp_set_pcie_role_fn pcie_cb, void *user)
{
	struct json_object *root, *board, *exposes, *entry, *val;
	const char *type_str;
	int len, rc = 0;

	if (!cfg || !filename)
		return -1;

	root = json_object_from_file(filename);
	if (!root) {
		warnx("JSON config file not found at %s, skipping", filename);
		return -1;
	}

	if (!json_object_is_type(root, json_type_array) ||
	    json_object_array_length(root) == 0) {
		warnx("Invalid JSON config format in %s", filename);
		json_object_put(root);
		return -1;
	}

	board = json_object_array_get_idx(root, 0);
	if (!json_object_object_get_ex(board, "Exposes", &exposes)) {
		warnx("No Exposes array in JSON config %s", filename);
		json_object_put(root);
		return -1;
	}

	len = json_object_array_length(exposes);
	for (int i = 0; i < len; i++) {
		entry = json_object_array_get_idx(exposes, i);
		struct json_object *type_obj = NULL;
		if (!json_object_object_get_ex(entry, "Type", &type_obj))
			continue;
		type_str = json_object_get_string(type_obj);

		if (strcmp(type_str, "MCTPI3CTarget") == 0) {
			/* Override semantics: if a device with the same Name
			 * was already loaded by TOML/compile-time defaults,
			 * replace it in place; otherwise append.
			 */
			const char *name_str = NULL;
			if (json_object_object_get_ex(entry, "Name", &val))
				name_str = json_object_get_string(val);

			int slot = (name_str ? i3c_find_by_name(cfg, name_str) : -1);
			if (slot < 0) {
				if (cfg->num_devices >= MAX_I3C_DEVICES) {
					warnx("JSON: too many I3C devices, skipping %s",
					      name_str ? name_str : "(unnamed)");
					rc = -1;
					continue;
				}
				slot = cfg->num_devices;
			}

			struct i3c_device *dev = &cfg->devices[slot];
			i3c_clear_regex(dev);
			memset(dev, 0, sizeof(*dev));

			if (name_str) {
				strncpy(dev->name, name_str, sizeof(dev->name) - 1);
			}
			if (json_object_object_get_ex(entry, "Bus", &val)) {
				dev->bus_num = json_object_get_int(val);
			}

			/* Read Role field, default to "endpoint" */
			const char *role_str = "endpoint";
			if (json_object_object_get_ex(entry, "Role", &val)) {
				role_str = json_object_get_string(val);
			}
			strncpy(dev->role, role_str, sizeof(dev->role) - 1);

			/* Read I3CTarget, default based on role */
			if (json_object_object_get_ex(entry, "I3CTarget", &val)) {
				dev->is_i3c_target = json_object_get_boolean(val);
			} else {
				dev->is_i3c_target = (strcmp(dev->role, "bus-owner") != 0);
			}

			if (json_object_object_get_ex(entry, "PidMask", &val)) {
				if (i3c_set_pid_mask(dev,
						     json_object_get_string(val)) < 0)
					rc = -1;
			}
			if (json_object_object_get_ex(entry, "SecondaryBusOwner", &val)) {
				dev->is_secondary_bus_owner =
					json_object_get_boolean(val);
			}

			/* Endpoint: read Address (6-byte PID array).
			 * Cap to MCTP_I3C_PHYS_ADDR_LEN (the schema also
			 * limits to 6) to avoid overrun of phys_addr.
			 */
			if (json_object_object_get_ex(entry, "Address", &val)) {
				int addr_len = json_object_array_length(val);
				const int cap = MCTP_I3C_PHYS_ADDR_LEN <
							(int)sizeof(dev->phys_addr) ?
							MCTP_I3C_PHYS_ADDR_LEN :
							(int)sizeof(dev->phys_addr);
				if (addr_len > cap)
					addr_len = cap;
				for (int j = 0; j < addr_len; j++) {
					dev->phys_addr[j] = (uint8_t)json_object_get_int(
						json_object_array_get_idx(val, j));
				}
			}

			/* Read StaticEndpointID if present */
			if (json_object_object_get_ex(entry, "StaticEndpointID", &val)) {
				dev->static_eid = (mctp_eid_t)strtoul(
					json_object_get_string(val), NULL, 0);
			}

			/* Use PhysicalLinkName if provided, otherwise
			 * fall back to generating from bus_num. */
			if (json_object_object_get_ex(entry, "PhysicalLinkName", &val)) {
				strncpy(dev->interface,
					json_object_get_string(val),
					INTERFACE_NAME_LEN);
				dev->interface[INTERFACE_NAME_LEN] = '\0';
			} else if (!dev->is_i3c_target) {
				snprintf(dev->interface, INTERFACE_NAME_LEN,
					 "mctpi3c%d", dev->bus_num);
			} else {
				snprintf(dev->interface, INTERFACE_NAME_LEN,
					 "mctpi3c-target%d", dev->bus_num);
			}

			warnx("JSON I3C device[%d]: name=%s bus=%d role=%s is_i3c_target=%d iface=%s pid_mask=0x%04X regex='%s'",
			      slot, dev->name, dev->bus_num, dev->role,
			      dev->is_i3c_target, dev->interface, dev->pid_mask,
			      dev->pid_regex_valid ? dev->pid_regex_str : "");

			if (slot == cfg->num_devices)
				cfg->num_devices++;
		} else if (strcmp(type_str, "MCTPPCIeConfiguration") == 0) {
			/* Each entry represents one PCIe interface. */
			char name[32] = {0}, link[INTERFACE_NAME_LEN + 1] = {0};
			char role[32] = {0};
			bool enabled = true; /* default: enabled */

			if (json_object_object_get_ex(entry, "Name", &val))
				strncpy(name, json_object_get_string(val), sizeof(name) - 1);
			if (json_object_object_get_ex(entry, "Enabled", &val))
				enabled = (strcmp(json_object_get_string(val), "enabled") == 0);
			if (json_object_object_get_ex(entry, "PhysicalLinkName", &val))
				strncpy(link, json_object_get_string(val), sizeof(link) - 1);
			if (json_object_object_get_ex(entry, "Role", &val)) {
				strncpy(role, json_object_get_string(val), sizeof(role) - 1);
				if (pcie_cb && pcie_cb(user, role) != 0) {
					warnx("JSON: invalid PCIe role '%s'", role);
					rc = -1;
				}
			}
			if (pcie_cfg) {
				if (pcie_cfg->num_interfaces >= MAX_PCIE_INTERFACES) {
					warnx("JSON: too many MCTPPCIeConfiguration entries, skipping '%s'", name);
				} else {
					struct pcie_interface_config *iface =
						&pcie_cfg->interfaces[pcie_cfg->num_interfaces++];
					strncpy(iface->name,      name, sizeof(iface->name) - 1);
					strncpy(iface->interface, link, sizeof(iface->interface) - 1);
					strncpy(iface->role,      role, sizeof(iface->role) - 1);
					iface->enabled = enabled;
					warnx("JSON PCIe iface[%d]: name=%s link=%s role=%s enabled=%d",
					      pcie_cfg->num_interfaces - 1,
					      iface->name, iface->interface, iface->role,
					      iface->enabled);
				}
			}
		}
	}

	json_object_put(root);
	warnx("JSON config loaded from %s: %d I3C device(s)",
	      filename, cfg->num_devices);
	return rc;
}
