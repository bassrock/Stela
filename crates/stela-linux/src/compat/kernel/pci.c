// SPDX-License-Identifier: GPL-2.0-only
/* Linux's PCI core. Stela's own PCI owners enumerate, power and map
 * functions; Linux drivers match against their tables and reach the function
 * through the pci_ops the owner gives its bus. */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/bits.h>
#include <linux/io.h>
#include <linux/log2.h>
#include <linux/pci.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <stela/device.h>
#include <stela/linux.h>
#include <stela/pci.h>
#include <stela/catalogue.h>
#include <unsupported.h>

/* An ID field matches its value or, as PCI_ANY_ID, any value. */
static bool id_matches(u32 id, u32 value)
{
	return id == (u32)PCI_ANY_ID || id == value;
}

/* The first entry before the table's all-zero terminator that matches. */
const struct pci_device_id *pci_match_id(const struct pci_device_id *ids, struct pci_dev *dev)
{
	for (; ids && (ids->vendor || ids->subvendor || ids->class_mask); ids++)
		if (id_matches(ids->vendor, dev->vendor) && id_matches(ids->device, dev->device) &&
		    id_matches(ids->subvendor, dev->subsystem_vendor) &&
		    id_matches(ids->subdevice, dev->subsystem_device) &&
		    !((ids->class ^ dev->class) & ids->class_mask))
			return ids;
	return NULL;
}

/* Managed enabling and wake configuration, which no selected driver uses. */
STELA_UNSUPPORTED(int, pcim_enable_device, struct pci_dev *)
STELA_UNSUPPORTED(int, pci_wait_for_pending_transaction, struct pci_dev *)
STELA_UNSUPPORTED(int, pci_wake_from_d3, struct pci_dev *, bool)
/* A function reset, which the owner does not offer and amdgpu reaches only
 * through its `pci` reset method; a message's register restore, which only
 * MI300's power table reaches; and the search by class, which only APUs and
 * the ATPX handler use. */
STELA_UNSUPPORTED(int, pci_reset_function, struct pci_dev *)
STELA_UNSUPPORTED(void, pci_restore_msi_state, struct pci_dev *)
STELA_UNSUPPORTED(struct pci_dev *, pci_get_class, unsigned int, struct pci_dev *)

/* Configuration space, through the bus's pci_ops, following
 * drivers/pci/access.c. Linux code runs in one context at a time, so
 * Linux's pci_lock has nothing to exclude. */
#define PCI_OP_READ(size, type, length, bad)                                                     \
	int pci_bus_read_config_##size(struct pci_bus *bus, unsigned int devfn, int pos,         \
				       type *value)                                               \
	{                                                                                        \
		u32 data = 0;                                                                    \
		if ((bad) || pos < 0 || pos > PCI_CFG_SPACE_EXP_SIZE - length)                                                                         \
			return PCIBIOS_BAD_REGISTER_NUMBER;                                      \
		int error = bus->ops->read(bus, devfn, pos, length, &data);                      \
		if (error)                                                                       \
			PCI_SET_ERROR_RESPONSE(value);                                           \
		else                                                                             \
			*value = (type)data;                                                     \
		return error;                                                                    \
	}                                                                                        \
	int pci_read_config_##size(const struct pci_dev *dev, int where, type *value)            \
	{                                                                                        \
		if (pci_dev_is_disconnected(dev)) {                                              \
			PCI_SET_ERROR_RESPONSE(value);                                           \
			return PCIBIOS_DEVICE_NOT_FOUND;                                         \
		}                                                                                \
		return pci_bus_read_config_##size(dev->bus, dev->devfn, where, value);           \
	}

#define PCI_OP_WRITE(size, type, length, bad)                                                    \
	int pci_bus_write_config_##size(struct pci_bus *bus, unsigned int devfn, int pos,        \
					type value)                                               \
	{                                                                                        \
		if ((bad) || pos < 0 || pos > PCI_CFG_SPACE_EXP_SIZE - length)                                                                         \
			return PCIBIOS_BAD_REGISTER_NUMBER;                                      \
		return bus->ops->write(bus, devfn, pos, length, value);                          \
	}                                                                                        \
	int pci_write_config_##size(const struct pci_dev *dev, int where, type value)            \
	{                                                                                        \
		if (pci_dev_is_disconnected(dev))                                                \
			return PCIBIOS_DEVICE_NOT_FOUND;                                         \
		return pci_bus_write_config_##size(dev->bus, dev->devfn, where, value);          \
	}

PCI_OP_READ(byte, u8, 1, false)
PCI_OP_READ(word, u16, 2, pos & 1)
PCI_OP_READ(dword, u32, 4, pos & 3)
PCI_OP_WRITE(byte, u8, 1, false)
PCI_OP_WRITE(word, u16, 2, pos & 1)
PCI_OP_WRITE(dword, u32, 4, pos & 3)

/* The standard capability list's entry for cap, or 0, as drivers/pci/pci.c
 * finds it; the bound stops a looped list. */
int stela_linux_pci_find_capability(struct pci_dev *dev, int cap, u8 *found)
{
    u16 status, entry;
    u8 pos;
    int error;
    *found = 0;
    error = pci_read_config_word(dev, PCI_STATUS, &status);
    if (error)
        return error;
    if (!(status & PCI_STATUS_CAP_LIST))
        return PCIBIOS_SUCCESSFUL;
    switch (dev->hdr_type) {
    case PCI_HEADER_TYPE_NORMAL:
    case PCI_HEADER_TYPE_BRIDGE:
        pos = PCI_CAPABILITY_LIST;
        break;
    case PCI_HEADER_TYPE_CARDBUS:
        pos = PCI_CB_CAPABILITY_LIST;
        break;
    default:
        return PCIBIOS_SUCCESSFUL;
    }
    error = pci_read_config_byte(dev, pos, &pos);
    if (error)
        return error;
    for (unsigned int bound = 48; bound && pos >= 0x40; bound--) {
        pos &= ~3;
        error = pci_read_config_word(dev, pos, &entry);
        if (error)
            return error;
        if ((entry & 0xff) == 0xff)
            break;
        if ((entry & 0xff) == cap) {
            *found = pos;
            break;
        }
        pos = (u8)(entry >> 8);
    }
    return PCIBIOS_SUCCESSFUL;
}

u8 pci_find_capability(struct pci_dev *dev, int cap)
{
    u8 found;
    if (stela_linux_pci_find_capability(dev, cap, &found))
        stela_linux_fatal("PCI capability access failed without a result");
    return found;
}

/* The configuration space's size, following drivers/pci/probe.c for a
 * function that is no virtual function, without quirks: a host bridge, a
 * PCI Express function or a PCI-X 266/533 one has the extended space when
 * its first dword reads as other than an error. */
static int pci_cfg_space_size_ext(struct pci_dev *dev)
{
	u32 status;

	if (pci_read_config_dword(dev, PCI_CFG_SPACE_SIZE, &status) != PCIBIOS_SUCCESSFUL)
		return PCI_CFG_SPACE_SIZE;
	if (PCI_POSSIBLE_ERROR(status))
		return PCI_CFG_SPACE_SIZE;
	return PCI_CFG_SPACE_EXP_SIZE;
}

int pci_cfg_space_size(struct pci_dev *dev)
{
	u32 status;
	u8 pos;

	if (dev->bus->bus_flags & PCI_BUS_FLAGS_NO_EXTCFG)
		return PCI_CFG_SPACE_SIZE;
	if ((dev->class >> 8) == PCI_CLASS_BRIDGE_HOST || pci_is_pcie(dev))
		return pci_cfg_space_size_ext(dev);
	if (stela_linux_pci_find_capability(dev, PCI_CAP_ID_PCIX, &pos) || !pos)
		return PCI_CFG_SPACE_SIZE;
	/* A failed read leaves all ones, as Linux's does. */
	pci_read_config_dword(dev, pos + PCI_X_STATUS, &status);
	if (status & (PCI_X_STATUS_266MHZ | PCI_X_STATUS_533MHZ))
		return pci_cfg_space_size_ext(dev);
	return PCI_CFG_SPACE_SIZE;
}

/* The extended capability list's entry for cap, or 0, following
 * drivers/pci/pci.c: none in a space that ends at the standard header; an
 * empty header ends the list and the bound stops a looped one. */
u16 pci_find_ext_capability(struct pci_dev *dev, int cap)
{
	u16 pos = PCI_CFG_SPACE_SIZE;
	u32 header;

	if (dev->cfg_size <= PCI_CFG_SPACE_SIZE)
		return 0;
	for (int ttl = (PCI_CFG_SPACE_EXP_SIZE - PCI_CFG_SPACE_SIZE) / 8;
	     ttl-- > 0 && pos >= PCI_CFG_SPACE_SIZE;) {
		if (pci_read_config_dword(dev, pos, &header) != PCIBIOS_SUCCESSFUL)
			break;
		if (header == 0)
			break;
		if (PCI_EXT_CAP_ID(header) == (u32)cap)
			return pos;
		pos = (u16)PCI_EXT_CAP_NEXT(header);
	}
	return 0;
}

/* PCI Express capability registers, following drivers/pci/access.c: a
 * register the function's port type or capability version leaves out reads
 * as zero without an access. The port type and version are the flags the
 * attach cached. */
static int pcie_cap_version(const struct pci_dev *dev)
{
	return pcie_caps_reg(dev) & PCI_EXP_FLAGS_VERS;
}

static bool pcie_downstream_port(const struct pci_dev *dev)
{
	int type = pci_pcie_type(dev);

	return type == PCI_EXP_TYPE_ROOT_PORT || type == PCI_EXP_TYPE_DOWNSTREAM ||
	       type == PCI_EXP_TYPE_PCIE_BRIDGE;
}

static bool pcie_cap_has_lnkctl(const struct pci_dev *dev)
{
	int type = pci_pcie_type(dev);

	return type == PCI_EXP_TYPE_ENDPOINT || type == PCI_EXP_TYPE_LEG_END ||
	       type == PCI_EXP_TYPE_ROOT_PORT || type == PCI_EXP_TYPE_UPSTREAM ||
	       type == PCI_EXP_TYPE_DOWNSTREAM || type == PCI_EXP_TYPE_PCI_BRIDGE ||
	       type == PCI_EXP_TYPE_PCIE_BRIDGE;
}

static bool pcie_cap_has_lnkctl2(const struct pci_dev *dev)
{
	return pcie_cap_has_lnkctl(dev) && pcie_cap_version(dev) > 1;
}

static bool pcie_cap_has_sltctl(const struct pci_dev *dev)
{
	return pcie_downstream_port(dev) && (pcie_caps_reg(dev) & PCI_EXP_FLAGS_SLOT);
}

static bool pcie_cap_has_rtctl(const struct pci_dev *dev)
{
	int type = pci_pcie_type(dev);

	return type == PCI_EXP_TYPE_ROOT_PORT || type == PCI_EXP_TYPE_RC_EC;
}

static bool pcie_capability_reg_implemented(struct pci_dev *dev, int pos)
{
	if (!pci_is_pcie(dev))
		return false;
	switch (pos) {
	case PCI_EXP_FLAGS:
	case PCI_EXP_DEVCAP:
	case PCI_EXP_DEVCTL:
	case PCI_EXP_DEVSTA:
		return true;
	case PCI_EXP_LNKCAP:
	case PCI_EXP_LNKCTL:
	case PCI_EXP_LNKSTA:
		return pcie_cap_has_lnkctl(dev);
	case PCI_EXP_SLTCAP:
	case PCI_EXP_SLTCTL:
	case PCI_EXP_SLTSTA:
		return pcie_cap_has_sltctl(dev);
	case PCI_EXP_RTCTL:
	case PCI_EXP_RTCAP:
	case PCI_EXP_RTSTA:
		return pcie_cap_has_rtctl(dev);
	case PCI_EXP_DEVCAP2:
	case PCI_EXP_DEVCTL2:
		return pcie_cap_version(dev) > 1;
	case PCI_EXP_LNKCAP2:
	case PCI_EXP_LNKCTL2:
	case PCI_EXP_LNKSTA2:
		return pcie_cap_has_lnkctl2(dev);
	default:
		return false;
	}
}

/* A downstream port without a slot reads its presence as detected
 * (PCIe Base Spec 3.0, sec 7.8). */
static bool pcie_presence_hardwired(struct pci_dev *dev, int pos)
{
	return pci_is_pcie(dev) && pcie_downstream_port(dev) && pos == PCI_EXP_SLTSTA;
}

int pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val)
{
	*val = 0;
	if (pos & 1)
		return PCIBIOS_BAD_REGISTER_NUMBER;
	if (pcie_capability_reg_implemented(dev, pos)) {
		int error = pci_read_config_word(dev, pci_pcie_cap(dev) + pos, val);

		if (error)
			*val = 0;
		return error;
	}
	if (pcie_presence_hardwired(dev, pos))
		*val = PCI_EXP_SLTSTA_PDS;
	return 0;
}

int pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val)
{
	*val = 0;
	if (pos & 3)
		return PCIBIOS_BAD_REGISTER_NUMBER;
	if (pcie_capability_reg_implemented(dev, pos)) {
		int error = pci_read_config_dword(dev, pci_pcie_cap(dev) + pos, val);

		if (error)
			*val = 0;
		return error;
	}
	if (pcie_presence_hardwired(dev, pos))
		*val = PCI_EXP_SLTSTA_PDS;
	return 0;
}

/* The payload and link capabilities, following drivers/pci/pci.c. A function
 * without the registers reports 128 bytes and an unknown link. */
int pcie_get_mps(struct pci_dev *dev)
{
	u16 control;

	pcie_capability_read_word(dev, PCI_EXP_DEVCTL, &control);
	return 128 << ((control & PCI_EXP_DEVCTL_PAYLOAD) >> 5);
}

/* The supported link speeds vector, which the attach caches as Linux's probe
 * does: Link Capabilities 2's speeds up to the Max Link Speed, or before
 * r3.0 the speeds that maximum implies. */
u8 pcie_get_supported_speeds(struct pci_dev *dev)
{
	u32 lnkcap2, lnkcap;
	u8 speeds;

	pcie_capability_read_dword(dev, PCI_EXP_LNKCAP2, &lnkcap2);
	speeds = (u8)(lnkcap2 & PCI_EXP_LNKCAP2_SLS);
	pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &lnkcap);
	speeds = (u8)(speeds & GENMASK(lnkcap & PCI_EXP_LNKCAP_SLS, 0));
	if (speeds)
		return speeds;
	if ((lnkcap & PCI_EXP_LNKCAP_SLS) == PCI_EXP_LNKCAP_SLS_5_0GB)
		speeds = PCI_EXP_LNKCAP2_SLS_5_0GB | PCI_EXP_LNKCAP2_SLS_2_5GB;
	else if ((lnkcap & PCI_EXP_LNKCAP_SLS) == PCI_EXP_LNKCAP_SLS_2_5GB)
		speeds = PCI_EXP_LNKCAP2_SLS_2_5GB;
	return speeds;
}

enum pci_bus_speed pcie_get_speed_cap(struct pci_dev *dev)
{
	u8 speeds = dev->supported_speeds;

	if (speeds & PCI_EXP_LNKCAP2_SLS_64_0GB)
		return PCIE_SPEED_64_0GT;
	if (speeds & PCI_EXP_LNKCAP2_SLS_32_0GB)
		return PCIE_SPEED_32_0GT;
	if (speeds & PCI_EXP_LNKCAP2_SLS_16_0GB)
		return PCIE_SPEED_16_0GT;
	if (speeds & PCI_EXP_LNKCAP2_SLS_8_0GB)
		return PCIE_SPEED_8_0GT;
	if (speeds & PCI_EXP_LNKCAP2_SLS_5_0GB)
		return PCIE_SPEED_5_0GT;
	if (speeds & PCI_EXP_LNKCAP2_SLS_2_5GB)
		return PCIE_SPEED_2_5GT;
	return PCI_SPEED_UNKNOWN;
}

enum pcie_link_width pcie_get_width_cap(struct pci_dev *dev)
{
	u32 lnkcap;

	pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &lnkcap);
	if (lnkcap)
		return (enum pcie_link_width)((lnkcap & PCI_EXP_LNKCAP_MLW) >> 4);
	return PCIE_LNK_WIDTH_UNKNOWN;
}

/* AtomicOp requests, following drivers/pci/pci.c: an endpoint may make them
 * when its root port completes them and every switch port on the way routes
 * them. The owner presents no bridge above a function, so no root port is
 * found and, as under a hypervisor that hides it, requests are on only if
 * the firmware turned them on. */
int pci_enable_atomic_ops_to_root(struct pci_dev *dev, u32 cap_mask)
{
	u32 ctl2;

	(void)cap_mask;
	if (dev->is_virtfn || !pci_is_pcie(dev))
		return -EINVAL;
	switch (pci_pcie_type(dev)) {
	case PCI_EXP_TYPE_ENDPOINT:
	case PCI_EXP_TYPE_LEG_END:
		break;
	default:
		return -EINVAL;
	}
	if (pcie_find_root_port(dev))
		stela_linux_fatal("PCI AtomicOp routing through a presented bridge");
	pcie_capability_read_dword(dev, PCI_EXP_DEVCTL2, &ctl2);
	return ctl2 & PCI_EXP_DEVCTL2_ATOMIC_REQ ? 0 : -EINVAL;
}

/* Resizable BARs, following drivers/pci/rebar.c: a size is a power of two
 * from 1 MB, encoded as its exponent above that. The capability is read
 * where the attach found it, as Linux's probe records it. */
#define PCI_REBAR_MIN_SIZE ((resource_size_t)SZ_1M)

int pci_rebar_bytes_to_size(u64 bytes)
{
	int rebar_minsize = ilog2(PCI_REBAR_MIN_SIZE);

	bytes = roundup_pow_of_two(bytes);
	return max(ilog2(bytes), rebar_minsize) - rebar_minsize;
}

/* The capability's control register for bar. A failed read leaves all ones,
 * as Linux's does. */
static int pci_rebar_find_pos(struct pci_dev *pdev, int bar)
{
	unsigned int pos = pdev->rebar_cap, bars;
	u32 control;

	if (!pos)
		return -ENOTSUPP;
	pci_read_config_dword(pdev, (int)pos + PCI_REBAR_CTRL, &control);
	bars = (control & PCI_REBAR_CTRL_NBAR_MASK) >> PCI_REBAR_CTRL_NBAR_SHIFT;
	for (unsigned int i = 0; i < bars; i++, pos += 8) {
		pci_read_config_dword(pdev, (int)pos + PCI_REBAR_CTRL, &control);
		if ((int)(control & PCI_REBAR_CTRL_BAR_IDX) == bar)
			return (int)pos;
	}
	return -ENOENT;
}

u64 pci_rebar_get_possible_sizes(struct pci_dev *pdev, int bar)
{
	int pos = pci_rebar_find_pos(pdev, bar);
	u32 cap;

	if (pos < 0)
		return 0;
	pci_read_config_dword(pdev, pos + PCI_REBAR_CAP, &cap);
	cap = (cap & PCI_REBAR_CAP_SIZES) >> 4;
	/* Sapphire RX 5600 XT Pulse has an invalid cap dword for BAR 0 */
	if (pdev->vendor == PCI_VENDOR_ID_ATI && pdev->device == 0x731f && bar == 0 &&
	    cap == 0x700)
		return 0x3f00;
	return cap;
}

int pci_rebar_get_max_size(struct pci_dev *pdev, int bar)
{
	u64 sizes = pci_rebar_get_possible_sizes(pdev, bar);

	if (!sizes)
		return -ENOENT;
	return (int)__fls(sizes);
}

/* A resize, which drivers/pci/rebar.c refuses when the host bridge must
 * preserve the firmware's assignment: the owner placed the function's
 * windows, and nothing reassigns them. */
int pci_resize_resource(struct pci_dev *dev, int i, int size, int exclude_bars)
{
	(void)dev;
	(void)i;
	(void)size;
	(void)exclude_bars;
	return -ENOTSUPP;
}

const struct bus_type pci_bus_type = {.name = "pci"};

/* The devices their owners registered and the drivers bound to them, as
 * drivers/pci/pci-driver.c matches and probes. */
struct registered_driver {
	struct list_head node;
	struct pci_driver *driver;
};

static LIST_HEAD(devices);
static LIST_HEAD(drivers);

static void bind(struct pci_driver *driver, struct pci_dev *dev)
{
	const struct pci_device_id *id = pci_match_id(driver->id_table, dev);

	if (dev->driver || !id)
		return;
	dev->driver = driver;
	dev->dev.driver = &driver->driver;
	int error = driver->probe(dev, id);

	if (!error)
		return;
	if (error != -ENODEV)
		dev_err(&dev->dev, "%s: probe failed: %pe\n", driver->name, ERR_PTR(error));
	stela_linux_devres_release(&dev->dev);
	dev->dev.driver = NULL;
	dev->driver = NULL;
}

static void unbind(struct pci_dev *dev)
{
	struct pci_driver *driver = dev->driver;

	if (!driver)
		return;
	if (driver->remove)
		driver->remove(dev);
	stela_linux_devres_release(&dev->dev);
	dev->dev.driver = NULL;
	dev->driver = NULL;
}

bool stela_linux_pci_devices_present(void)
{
	return !list_empty(&devices);
}

int stela_linux_pci_publish_device(struct pci_dev *dev)
{
	struct registered_driver *registered;
	if (!stela_linux_device_publish_allowed(&dev->dev))
		return -EPERM;

	dev->dev.bus = &pci_bus_type;
	list_add_tail(&dev->bus_list, &devices);
	list_for_each_entry(registered, &drivers, node)
		bind(registered->driver, dev);
	return 0;
}

bool stela_linux_pci_bound(const struct pci_dev *device)
{
	return device->dev.driver;
}

/* What the kernel's shutdown does of a device: its driver stops the hardware. */
void stela_linux_pci_shutdown(struct pci_dev *dev)
{
	if (dev->driver && dev->driver->shutdown)
		dev->driver->shutdown(dev);
}

void stela_linux_pci_remove_device(struct pci_dev *dev)
{
	stela_linux_device_close(&dev->dev);
	unbind(dev);
	list_del(&dev->bus_list);
	put_device(&dev->dev);
}

int __pci_register_driver(struct pci_driver *driver, struct module *owner, const char *name)
{
	struct registered_driver *registered = kmalloc_obj(*registered);
	struct pci_dev *dev;

	(void)owner;
	if (!registered)
		return -ENOMEM;
	driver->driver.name = driver->name;
	driver->driver.mod_name = name;
	registered->driver = driver;
	list_add_tail(&registered->node, &drivers);
	list_for_each_entry(dev, &devices, bus_list)
		bind(driver, dev);
	return 0;
}

void pci_unregister_driver(struct pci_driver *driver)
{
	struct registered_driver *registered;
	struct pci_dev *dev;

	list_for_each_entry(registered, &drivers, node)
		if (registered->driver == driver) {
			list_del(&registered->node);
			kfree(registered);
			break;
		}
	list_for_each_entry(dev, &devices, bus_list)
		if (dev->driver == driver)
			unbind(dev);
}

/* Owners keep their bridges' hotplug slots out of a driver's power cycles. */
void pci_ignore_hotplug(struct pci_dev *dev)
{
	dev->ignore_hotplug = 1;
}

/* The standard header, which is what a function loses in D3, following
 * drivers/pci/pci.c. */
int pci_save_state(struct pci_dev *dev)
{
    enum { SAVED_DWORDS = 16 };
    u32 saved[SAVED_DWORDS];
    int error;
    for (int i = 0; i < SAVED_DWORDS; i++) {
        error = pci_read_config_dword(dev, i * 4, &saved[i]);
        if (error)
            return pcibios_err_to_errno(error);
    }
    memcpy(dev->saved_config_space, saved, sizeof(saved));
    dev->state_saved = true;
    return 0;
}

void pci_restore_state(struct pci_dev *dev)
{
    if (!dev->state_saved)
        return;
    for (int i = 15; i >= 0; i--)
        if (pci_write_config_dword(dev, i * 4, dev->saved_config_space[i]))
            stela_linux_fatal("PCI state restore failed without completion");
    dev->state_saved = false;
}

/* A copy of the saved header a driver keeps across resets and frees,
 * following drivers/pci/pci.c; loading it makes it the state to restore.
 * pci_save_state above saves no capability state, so the copy is the
 * header alone. */
struct pci_saved_state {
	u32 config_space[16];
};

struct pci_saved_state *pci_store_saved_state(struct pci_dev *dev)
{
	struct pci_saved_state *state;

	if (!dev->state_saved)
		return NULL;
	state = kmalloc_obj(*state);
	if (!state)
		return NULL;
	memcpy(state->config_space, dev->saved_config_space, sizeof(state->config_space));
	return state;
}

int pci_load_saved_state(struct pci_dev *dev, struct pci_saved_state *state)
{
	dev->state_saved = false;
	if (!state)
		return 0;
	memcpy(dev->saved_config_space, state->config_space, sizeof(state->config_space));
	dev->state_saved = true;
	return 0;
}

/* The power management capability's D-state, with the settling time D3hot
 * transitions need. A function without the capability stays in D0. */
int pci_set_power_state(struct pci_dev *dev, pci_power_t state)
{
    u8 pm;
    u16 control;
    int error;
    if (state < PCI_D0 || state > PCI_D3hot)
        return -EINVAL;
    error = stela_linux_pci_find_capability(dev, PCI_CAP_ID_PM, &pm);
    if (error)
        return pcibios_err_to_errno(error);
    if (!pm)
        return state == PCI_D0 ? 0 : -EIO;
    error = pci_read_config_word(dev, pm + PCI_PM_CTRL, &control);
    if (error)
        return pcibios_err_to_errno(error);
    u16 current_state = control & PCI_PM_CTRL_STATE_MASK;
    u16 target = (__force u16)state;
    if (current_state == target)
        return 0;
    control = (control & (u16)~PCI_PM_CTRL_STATE_MASK) | target;
    error = pci_write_config_word(dev, pm + PCI_PM_CTRL, control);
    if (error)
        return pcibios_err_to_errno(error);
    if (current_state == (__force u16)PCI_D3hot || target == (__force u16)PCI_D3hot)
        msleep(10);
    dev->current_state = state;
    return 0;
}

struct pci_dev *pci_dev_get(struct pci_dev *dev)
{
	if (dev)
		get_device(&dev->dev);
	return dev;
}

void pci_dev_put(struct pci_dev *dev)
{
	if (dev)
		put_device(&dev->dev);
}

struct pci_dev *pci_get_domain_bus_and_slot(int domain, unsigned int bus, unsigned int devfn)
{
	struct pci_dev *dev;

	list_for_each_entry(dev, &devices, bus_list)
		if (pci_domain_nr(dev->bus) == domain && dev->bus->number == bus &&
		    dev->devfn == devfn)
			return pci_dev_get(dev);
	return NULL;
}

/* A bus's windows, following drivers/pci/bus.c: its bridge's, then those
 * added to its list. The owner's root bus has neither. */
struct pci_bus_resource {
	struct list_head list;
	struct resource *res;
};

struct resource *pci_bus_resource_n(const struct pci_bus *bus, int n)
{
	struct pci_bus_resource *bus_res;

	if (n < PCI_BRIDGE_RESOURCE_NUM)
		return bus->resource[n];
	n -= PCI_BRIDGE_RESOURCE_NUM;
	list_for_each_entry(bus_res, &bus->resources, list)
		if (n-- == 0)
			return bus_res->res;
	return NULL;
}

/* Enabling, following drivers/pci/pci.c. The owner has powered the function
 * and assigned its resources; this turns on their decoding. */
int pci_enable_device(struct pci_dev *dev)
{
    int error;
    u16 old, command;
    if (atomic_inc_return(&dev->enable_cnt) > 1)
        return 0;
    error = pci_read_config_word(dev, PCI_COMMAND, &old);
    if (error)
        goto refused;
    command = old;
    for (int bar = 0; bar < PCI_ROM_RESOURCE; bar++) {
        unsigned long flags = pci_resource_flags(dev, bar);
        if (flags & IORESOURCE_IO)
            command |= PCI_COMMAND_IO;
        else if (flags & IORESOURCE_MEM)
            command |= PCI_COMMAND_MEMORY;
    }
    if (command != old) {
        error = pci_write_config_word(dev, PCI_COMMAND, command);
        if (error)
            goto refused;
    }
    return 0;
refused:
    /* A possibly applied write traps at the actual Rust boundary. This only
     * restores metadata after a before-write PCIBIOS refusal; no hardware RMW. */
    atomic_dec(&dev->enable_cnt);
    return pcibios_err_to_errno(error);
}

void pci_disable_device(struct pci_dev *dev)
{
    WARN_ON_ONCE(atomic_read(&dev->enable_cnt) <= 0);
    if (atomic_dec_return(&dev->enable_cnt))
        return;
    if (dev->is_busmaster) {
        u16 command;
        if (pci_read_config_word(dev, PCI_COMMAND, &command) ||
            pci_write_config_word(dev, PCI_COMMAND, command & (u16)~PCI_COMMAND_MASTER))
            stela_linux_fatal("PCI disable failed without completion");
        dev->is_busmaster = 0;
    }
}

/* Linux's __pci_set_master reads Command only to compare the old and new
 * values, writes it and goes on whatever the function kept. A function may
 * drop the write (Bus Master Enable hardwired to zero, or a write the
 * function ignores), and its DMA then never reaches memory: this reads the
 * register back after the write and says so, then goes on as Linux does. */
void pci_set_master(struct pci_dev *dev)
{
    u16 command;
    if (pci_read_config_word(dev, PCI_COMMAND, &command))
        stela_linux_fatal("PCI bus-master access failed without completion");
    if (!(command & PCI_COMMAND_MASTER)) {
        if (pci_write_config_word(dev, PCI_COMMAND, command | PCI_COMMAND_MASTER) ||
            pci_read_config_word(dev, PCI_COMMAND, &command))
            stela_linux_fatal("PCI bus-master access failed without completion");
        if (!(command & PCI_COMMAND_MASTER))
            pci_warn(dev, "bus mastering stayed off (command %#06x): its DMA cannot reach memory\n",
                     command);
    }
    dev->is_busmaster = 1;
}

void pci_clear_master(struct pci_dev *dev)
{
	u16 command;

	if (pci_read_config_word(dev, PCI_COMMAND, &command))
		stela_linux_fatal("PCI bus-master access failed without completion");
	if ((command & PCI_COMMAND_MASTER) &&
	    pci_write_config_word(dev, PCI_COMMAND, command & (u16)~PCI_COMMAND_MASTER))
		stela_linux_fatal("PCI bus-master access failed without completion");
	dev->is_busmaster = 0;
}

/* The function's line, following drivers/pci/pci.c: the command
 * register's INTx disable bit. */
void pci_intx(struct pci_dev *pdev, int enable)
{
	u16 command, new;

	if (pci_read_config_word(pdev, PCI_COMMAND, &command))
		stela_linux_fatal("PCI INTx access failed without completion");
	if (enable)
		new = command & (u16)~PCI_COMMAND_INTX_DISABLE;
	else
		new = command | PCI_COMMAND_INTX_DISABLE;
	if (new != command && pci_write_config_word(pdev, PCI_COMMAND, new))
		stela_linux_fatal("PCI INTx access failed without completion");
}

/* Option ROMs, following drivers/pci/rom.c. The owner assigned the ROM
 * resource, whose CPU and bus addresses are one on x86. */
int pci_enable_rom(struct pci_dev *pdev)
{
    struct resource *res = &pdev->resource[PCI_ROM_RESOURCE];
    u32 address;
    int error;
    if (!res->flags)
        return -1;
    if (res->flags & IORESOURCE_ROM_SHADOW)
        return 0;
    error = pci_read_config_dword(pdev, pdev->rom_base_reg, &address);
    if (error)
        return pcibios_err_to_errno(error);
    address &= ~PCI_ROM_ADDRESS_MASK;
    address |= (u32)res->start | PCI_ROM_ADDRESS_ENABLE;
    error = pci_write_config_dword(pdev, pdev->rom_base_reg, address);
    return pcibios_err_to_errno(error);
}

void pci_disable_rom(struct pci_dev *pdev)
{
    struct resource *res = &pdev->resource[PCI_ROM_RESOURCE];
    u32 address;
    if (res->flags & IORESOURCE_ROM_SHADOW)
        return;
    if (pci_read_config_dword(pdev, pdev->rom_base_reg, &address))
        stela_linux_fatal("PCI ROM disable failed without completion");
    address &= (u32)~PCI_ROM_ADDRESS_ENABLE;
    if (pci_write_config_dword(pdev, pdev->rom_base_reg, address))
        stela_linux_fatal("PCI ROM disable failed without completion");
}

/* The bytes the ROM's images span, never more than its window: real ROMs
 * have windows far larger than their contents, and some misreport. */
static size_t rom_size(struct pci_dev *pdev, void __iomem *rom, size_t size)
{
	void __iomem *image = rom;
	unsigned int length;
	int last_image;

	do {
		if (readw(image) != 0xaa55) {
			pci_info(pdev, "Invalid PCI ROM header signature: got %#06x\n",
				 readw(image));
			break;
		}
		void __iomem *data = image + readw(image + 24);
		if (readl(data) != 0x52494350) {
			pci_info(pdev, "Invalid PCI ROM data signature: got %#010x\n",
				 readl(data));
			break;
		}
		last_image = readb(data + 21) & 0x80;
		length = readw(data + 16);
		image += length * 512;
		if (image >= rom + size)
			break;
		if (!last_image && readw(image) != 0xaa55)
			break;
	} while (length && !last_image);
	return min((size_t)(image - rom), size);
}

void __iomem *pci_map_rom(struct pci_dev *pdev, size_t *size)
{
	resource_size_t start = pci_resource_start(pdev, PCI_ROM_RESOURCE);

	*size = (size_t)pci_resource_len(pdev, PCI_ROM_RESOURCE);
	if (!*size || pci_enable_rom(pdev))
		return NULL;
	void __iomem *rom = ioremap(start, *size);
	if (rom) {
		*size = rom_size(pdev, rom, *size);
		if (*size)
			return rom;
		iounmap(rom);
	}
	pci_disable_rom(pdev);
	return NULL;
}

void pci_unmap_rom(struct pci_dev *pdev, void __iomem *rom)
{
	iounmap(rom);
	pci_disable_rom(pdev);
}

#ifdef STELA_LINUX_PCI_TEST
#include <stela/linux.h>
#include <stela/test.h>

static u8 config[PCI_CFG_SPACE_EXP_SIZE];
static int config_error;
static bool ignore_bus_master_write;
static unsigned int warnings;
static u8 rom_window[0x1000] __aligned(PAGE_SIZE);

static int read_config(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 *value)
{
	(void)bus;
	if (devfn != PCI_DEVFN(1, 0))
		return PCIBIOS_DEVICE_NOT_FOUND;
	if (config_error)
		return config_error;
	*value = 0;
	memcpy(value, &config[where], (size_t)size);
	return 0;
}

static int write_config(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 value)
{
	(void)bus;
	(void)devfn;
	if (ignore_bus_master_write && where == PCI_COMMAND && size == 2 &&
	    (value & PCI_COMMAND_MASTER))
		return 0;
	memcpy(&config[where], &value, (size_t)size);
	return 0;
}

static struct pci_ops ops = {.read = read_config, .write = write_config};

static void *map_device(void *context, u64 physical, size_t size,
			bool write_combining)
{
	(void)context;
	(void)write_combining;
	assert(physical == 0xc0000000 && size == sizeof(rom_window));
	return rom_window;
}

static void unmap_device(void *context, void *address)
{
	(void)context;
	assert(address == rom_window);
}

static void log_line(void *context, int level, const char *message)
{
	(void)context;
	if (level == LOGLEVEL_WARNING && strstr(message, "bus mastering stayed off (command 0x0002)"))
		warnings++;
}

static const struct stela_linux_services services = {
	.map_device = map_device,
	.unmap_device = unmap_device,
	.log = log_line,
};
static const struct stela_linux_owner owner = {.services = &services};

static void one_image(u8 *image, unsigned int blocks, bool last)
{
	image[0] = 0x55;
	image[1] = 0xaa;
	image[24] = 0x40;
	memcpy(image + 0x40, "PCIR", 4);
	image[0x40 + 16] = (u8)blocks;
	image[0x40 + 21] = last ? 0x80 : 0;
}

static struct pci_dev dev, absent;

/* An extended capability header: id, version 1, the next entry's offset. */
static void ext_capability(unsigned int pos, unsigned int id, unsigned int next)
{
	*(u32 *)&config[pos] = id | (1u << 16) | (next << 20);
}

int main(void)
{
	stela_linux_enter(&owner);
	struct pci_bus bus = {.ops = &ops, .number = 3};
	INIT_LIST_HEAD(&bus.resources);
	dev.bus = &bus;
	dev.devfn = PCI_DEVFN(1, 0);
	dev.hdr_type = PCI_HEADER_TYPE_NORMAL;
	dev.rom_base_reg = PCI_ROM_ADDRESS;

	u32 value;
	config[PCI_VENDOR_ID] = 0xde;
	config[PCI_VENDOR_ID + 1] = 0x10;
	assert(!pci_read_config_dword(&dev, 0, &value) && value == 0x10de);
	assert(pci_read_config_dword(&dev, 2, &value) == PCIBIOS_BAD_REGISTER_NUMBER);
	absent.bus = &bus;
	absent.devfn = PCI_DEVFN(2, 0);
	assert(pci_read_config_dword(&absent, 0, &value) == PCIBIOS_DEVICE_NOT_FOUND &&
	       value == ~0U);

	/* Capabilities at 0x50 then 0x60; a list that loops stops. */
	config[PCI_STATUS] = PCI_STATUS_CAP_LIST;
	config[PCI_CAPABILITY_LIST] = 0x50;
	config[0x50] = PCI_CAP_ID_PM;
	config[0x51] = 0x60;
	config[0x60] = PCI_CAP_ID_EXP;
    u8 checked_capability = 0xaa;
    config_error = PCIBIOS_SET_FAILED;
    assert(pci_read_config_dword(&dev, 0, &value) == PCIBIOS_SET_FAILED && value == ~0U);
    assert(stela_linux_pci_find_capability(&dev, PCI_CAP_ID_EXP, &checked_capability) == PCIBIOS_SET_FAILED && !checked_capability);
    dev.saved_config_space[0] = 0x12345678;
    assert(pci_save_state(&dev) == -EIO && !dev.state_saved && dev.saved_config_space[0] == 0x12345678);
    assert(pci_enable_device(&dev) == -EIO && !atomic_read(&dev.enable_cnt));
    assert(pci_set_power_state(&dev, PCI_D0) == -EIO);
    config_error = 0;
	assert(pci_find_capability(&dev, PCI_CAP_ID_EXP) == 0x60);
	assert(!pci_find_capability(&dev, PCI_CAP_ID_AGP));
	config[0x61] = 0x50;
	assert(!pci_find_capability(&dev, PCI_CAP_ID_AGP));

	dev.resource[0].flags = IORESOURCE_MEM;
	assert(!pci_enable_device(&dev) && !pci_enable_device(&dev));
	/* A function that drops the write: a warning, and on as Linux goes. */
	ignore_bus_master_write = true;
	pci_set_master(&dev);
	assert(config[PCI_COMMAND] == PCI_COMMAND_MEMORY && dev.is_busmaster && warnings == 1);
	ignore_bus_master_write = false;
	pci_set_master(&dev);
	assert(warnings == 1);
	assert(config[PCI_COMMAND] == (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER));
	pci_disable_device(&dev);
	assert(config[PCI_COMMAND] & PCI_COMMAND_MASTER);
	pci_disable_device(&dev);
	assert(config[PCI_COMMAND] == PCI_COMMAND_MEMORY && !dev.is_busmaster);

	device_initialize(&dev.dev);
	assert(!stela_linux_pci_publish_device(&dev));
	assert(pci_get_domain_bus_and_slot(0, 3, PCI_DEVFN(1, 0)) == &dev);
	pci_dev_put(&dev);
	assert(!pci_get_domain_bus_and_slot(0, 4, PCI_DEVFN(1, 0)));
	list_del(&dev.bus_list);

	/* Two images of a block each in a window of eight: the size is theirs. */
	dev.resource[PCI_ROM_RESOURCE].start = 0xc0000000;
	dev.resource[PCI_ROM_RESOURCE].end = 0xc0000fff;
	dev.resource[PCI_ROM_RESOURCE].flags = IORESOURCE_MEM;
	one_image(rom_window, 1, false);
	one_image(rom_window + 512, 1, true);
	size_t size;
	void __iomem *rom = pci_map_rom(&dev, &size);
	assert(rom == (void __iomem *)rom_window && size == 1024);
	assert(*(u32 *)&config[PCI_ROM_ADDRESS] == (0xc0000000 | PCI_ROM_ADDRESS_ENABLE));
	pci_unmap_rom(&dev, rom);
	assert(*(u32 *)&config[PCI_ROM_ADDRESS] == 0xc0000000);

	/* No ROM signature: nothing maps and decoding goes back off. */
	rom_window[0] = 0;
	assert(!pci_map_rom(&dev, &size) && *(u32 *)&config[PCI_ROM_ADDRESS] == 0xc0000000);

	/* PCI Express registers of a version 2 endpoint at 0x60: a payload of
	 * 256 bytes, an x16 link whose Max Link Speed of 16 GT/s bounds the
	 * speeds Link Capabilities 2 lists; the slot registers an endpoint
	 * lacks read as zero, an odd offset is refused. */
	dev.pcie_cap = 0x60;
	dev.pcie_flags_reg = 0x0002;
	config[0x60 + PCI_EXP_DEVCTL] = 0x20;
	*(u32 *)&config[0x60 + PCI_EXP_LNKCAP] = 0x104u;
	*(u32 *)&config[0x60 + PCI_EXP_LNKCAP2] = 0x3eu;
	u16 word = 0xffff;
	assert(!pcie_capability_read_word(&dev, PCI_EXP_DEVCTL, &word) && word == 0x20);
	assert(!pcie_capability_read_word(&dev, PCI_EXP_SLTSTA, &word) && !word);
	assert(pcie_capability_read_word(&dev, PCI_EXP_DEVCTL + 1, &word) ==
	       PCIBIOS_BAD_REGISTER_NUMBER);
	assert(pcie_get_mps(&dev) == 256);
	dev.supported_speeds = pcie_get_supported_speeds(&dev);
	assert(dev.supported_speeds == 0x1e && pcie_get_speed_cap(&dev) == PCIE_SPEED_16_0GT);
	assert(pcie_get_width_cap(&dev) == PCIE_LNK_X16);
	/* Before r3.0, the speeds the maximum of 5 GT/s implies. */
	*(u32 *)&config[0x60 + PCI_EXP_LNKCAP2] = 0;
	*(u32 *)&config[0x60 + PCI_EXP_LNKCAP] = 0x102u;
	dev.supported_speeds = pcie_get_supported_speeds(&dev);
	assert(dev.supported_speeds == 0x06 && pcie_get_speed_cap(&dev) == PCIE_SPEED_5_0GT);
	/* AtomicOps: with no root port presented, on only if the firmware set
	 * them; never for a port. */
	const u32 atomics = PCI_EXP_DEVCAP2_ATOMIC_COMP32 | PCI_EXP_DEVCAP2_ATOMIC_COMP64;
	assert(pci_enable_atomic_ops_to_root(&dev, atomics) == -EINVAL);
	assert(!config[0x60 + PCI_EXP_DEVCTL2]);
	config[0x60 + PCI_EXP_DEVCTL2] = PCI_EXP_DEVCTL2_ATOMIC_REQ;
	assert(!pci_enable_atomic_ops_to_root(&dev, atomics));
	/* A downstream port without a slot: presence detected, no access. */
	dev.pcie_flags_reg = 0x0062;
	assert(pci_enable_atomic_ops_to_root(&dev, atomics) == -EINVAL);
	assert(!pcie_capability_read_word(&dev, PCI_EXP_SLTSTA, &word) &&
	       word == PCI_EXP_SLTSTA_PDS);
	config[0x60 + PCI_EXP_DEVCTL2] = 0;
	dev.pcie_flags_reg = 0x0002;
	/* Without the capability, nothing reads: a 128-byte payload, an unknown link. */
	dev.pcie_cap = 0;
	dev.pcie_flags_reg = 0;
	assert(!pcie_capability_read_word(&dev, PCI_EXP_DEVCTL, &word) && !word);
	assert(pcie_get_mps(&dev) == 128 && pcie_get_width_cap(&dev) == PCIE_LNK_WIDTH_UNKNOWN);
	dev.supported_speeds = 0;
	assert(pcie_get_speed_cap(&dev) == PCI_SPEED_UNKNOWN);

	/* The extended space of a PCI Express function: there unless its first
	 * dword fails or reads as all ones; a conventional one has none. */
	dev.pcie_cap = 0x60;
	dev.pcie_flags_reg = 0x0002;
	assert(pci_cfg_space_size(&dev) == PCI_CFG_SPACE_EXP_SIZE);
	*(u32 *)&config[0x100] = ~0u;
	assert(pci_cfg_space_size(&dev) == PCI_CFG_SPACE_SIZE);
	config_error = PCIBIOS_SET_FAILED;
	assert(pci_cfg_space_size(&dev) == PCI_CFG_SPACE_SIZE);
	config_error = 0;
	dev.cfg_size = PCI_CFG_SPACE_SIZE;
	assert(!pci_find_ext_capability(&dev, PCI_EXT_CAP_ID_VNDR));

	/* Extended capabilities: vendor-specific at 0x100, Resizable BAR at
	 * 0x150 with BAR 0 up to 16 GB. An empty first header means none; a
	 * looped list stops. Nothing resizes. */
	ext_capability(0x100, PCI_EXT_CAP_ID_VNDR, 0x150);
	ext_capability(0x150, PCI_EXT_CAP_ID_REBAR, 0);
	*(u32 *)&config[0x150 + PCI_REBAR_CAP] = 0x7fffu << 4;
	*(u32 *)&config[0x150 + PCI_REBAR_CTRL] = 1u << PCI_REBAR_CTRL_NBAR_SHIFT;
	dev.cfg_size = pci_cfg_space_size(&dev);
	assert(dev.cfg_size == PCI_CFG_SPACE_EXP_SIZE);
	assert(pci_find_ext_capability(&dev, PCI_EXT_CAP_ID_VNDR) == 0x100);
	assert(pci_find_ext_capability(&dev, PCI_EXT_CAP_ID_REBAR) == 0x150);
	assert(!pci_find_ext_capability(&dev, PCI_EXT_CAP_ID_LTR));
	assert(pci_rebar_bytes_to_size(1) == 0 && pci_rebar_bytes_to_size((256u << 20) + 1) == 9);
	assert(pci_rebar_bytes_to_size(16ull << 30) == 14);
	assert(!pci_rebar_get_possible_sizes(&dev, 0));
	assert(pci_rebar_get_max_size(&dev, 0) == -ENOENT);
	dev.rebar_cap = pci_find_ext_capability(&dev, PCI_EXT_CAP_ID_REBAR);
	assert(pci_rebar_get_possible_sizes(&dev, 0) == 0x7fffu);
	assert(pci_rebar_get_max_size(&dev, 0) == 14 && pci_rebar_get_max_size(&dev, 2) == -ENOENT);
	assert(pci_resize_resource(&dev, 0, 14, 1 << 5) == -ENOTSUPP);
	ext_capability(0x100, PCI_EXT_CAP_ID_VNDR, 0x100);
	assert(!pci_find_ext_capability(&dev, PCI_EXT_CAP_ID_LTR));
	*(u32 *)&config[0x100] = 0;
	assert(!pci_find_ext_capability(&dev, PCI_EXT_CAP_ID_VNDR));
	dev.rebar_cap = 0;
	dev.pcie_cap = 0;
	dev.pcie_flags_reg = 0;

	/* The saved header is stored and loaded; nothing saved stores nothing. */
	dev.state_saved = false;
	assert(!pci_store_saved_state(&dev));
	assert(!pci_save_state(&dev));
	struct pci_saved_state *saved = pci_store_saved_state(&dev);

	assert(saved && !memcmp(saved->config_space, dev.saved_config_space,
				sizeof(saved->config_space)));
	dev.saved_config_space[1] = 0xdeadbeef;
	dev.state_saved = false;
	assert(!pci_load_saved_state(&dev, saved) && dev.state_saved);
	assert(dev.saved_config_space[1] == saved->config_space[1]);
	assert(!pci_load_saved_state(&dev, NULL) && !dev.state_saved);
	kfree(saved);

	/* A bus's windows: its bridge's, then its list; this root has none. */
	struct resource *res;
	unsigned int i;

	pci_bus_for_each_resource(&bus, res, i)
		assert(!res);
	assert(i == PCI_BRIDGE_RESOURCE_NUM && !res);
	struct resource window = {.start = 0x200000000ull, .end = 0x2ffffffffull,
				  .flags = IORESOURCE_MEM | IORESOURCE_MEM_64};
	struct pci_bus_resource extra = {.res = &window};

	bus.resource[1] = &window;
	list_add_tail(&extra.list, &bus.resources);
	assert(pci_bus_resource_n(&bus, 1) == &window && !pci_bus_resource_n(&bus, 2));
	assert(pci_bus_resource_n(&bus, PCI_BRIDGE_RESOURCE_NUM) == &window);
	assert(!pci_bus_resource_n(&bus, PCI_BRIDGE_RESOURCE_NUM + 1));
	bus.resource[1] = NULL;
	list_del(&extra.list);

	/* Bus mastering off: the command bit and the device's record. */
	pci_set_master(&dev);
	assert(dev.is_busmaster && (config[PCI_COMMAND] & PCI_COMMAND_MASTER));
	pci_clear_master(&dev);
	assert(!dev.is_busmaster && !(config[PCI_COMMAND] & PCI_COMMAND_MASTER));

	/* The line: the INTx disable bit, cleared to enable and set to disable. */
	pci_intx(&dev, 0);
	assert(*(u16 *)&config[PCI_COMMAND] & PCI_COMMAND_INTX_DISABLE);
	pci_intx(&dev, 1);
	assert(!(*(u16 *)&config[PCI_COMMAND] & PCI_COMMAND_INTX_DISABLE));
	stela_linux_exit();
	printf("Linux PCI configuration, enabling, option ROMs, PCI Express registers, Resizable BARs and saved state passed\n");
	return 0;
}
#endif
