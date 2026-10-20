/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mctp-config.h - MCTP I3C/PCIe configuration loaders
 *
 * Loads MCTP I3C device + PCIe role configuration from either:
 *  - the EntityManager D-Bus service (xyz.openbmc_project.EntityManager),
 *  - or a JSON config file in the same EM-style schema.
 *
 * Both loaders share override-by-name semantics: an existing entry in
 * @cfg with the same Name is replaced in place, otherwise a new slot
 * is appended (up to MAX_I3C_DEVICES).
 */
#ifndef MCTP_CONFIG_H
#define MCTP_CONFIG_H

#include <systemd/sd-bus.h>

#include "mctp-pid.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-interface PCIe MCTP configuration, loaded from each
 * MCTPPCIeConfiguration entry in the entity manager / JSON config.
 */
struct pcie_interface_config {
	char name[32];
	char interface[INTERFACE_NAME_LEN + 1]; /* physical link name, e.g. mctppci0 */
	char role[32];      /* "bus-owner" or "endpoint" */
	bool enabled;       /* false = interface disabled, skip MCTP setup */
};

#define MAX_PCIE_INTERFACES 8

/* Array of per-interface PCIe configs populated from entity manager. */
struct pcie_config {
	int num_interfaces;
	struct pcie_interface_config interfaces[MAX_PCIE_INTERFACES];
};

/* Callback invoked when a "Role" property is found on an
 * MCTPPCIeConfiguration entry. Return 0 on success, non-zero to
 * indicate the role was rejected.
 */
typedef int (*mctp_set_pcie_role_fn)(void *user, const char *role);

/* Load configuration from EntityManager via ObjectManager.GetManagedObjects.
 *
 * @cfg:      i3c_config to populate (devices appended/replaced).
 * @pcie_cfg: pcie_config to populate (interfaces appended per entry); may be NULL.
 * @bus:      optional pre-existing system bus connection. If NULL, a
 *            short-lived connection is opened internally.
 * @pcie_cb:  optional callback for PCIe role updates (may be NULL).
 * @user:     opaque pointer passed to @pcie_cb.
 *
 * Returns the number of MCTPI3CTarget entries loaded (>=0), or -1 on
 * a transport error (EM unreachable, malformed reply).
 */
int mctp_load_from_entity_manager(struct i3c_config *cfg,
				  struct pcie_config *pcie_cfg,
				  sd_bus *bus,
				  mctp_set_pcie_role_fn pcie_cb, void *user);

/* Load configuration from a JSON file in EM-style schema.
 *
 * @cfg:      i3c_config to populate.
 * @pcie_cfg: pcie_config to populate (interfaces appended per entry); may be NULL.
 * @filename: path to JSON config (e.g. MCTPD_JSON_FILE_DEFAULT).
 * @pcie_cb:  optional callback for PCIe role updates (may be NULL).
 * @user:     opaque pointer passed to @pcie_cb.
 *
 * Returns 0 on success, -1 if the file is missing/malformed or any
 * individual entry failed.
 */
int mctp_parse_json_file(struct i3c_config *cfg,
			 struct pcie_config *pcie_cfg,
			 const char *filename,
			 mctp_set_pcie_role_fn pcie_cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* MCTP_CONFIG_H */
