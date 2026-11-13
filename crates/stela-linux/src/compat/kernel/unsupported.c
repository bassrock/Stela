// SPDX-License-Identifier: GPL-2.0-or-later
/* Kernel services Stela does not provide yet. */
#include <linux/backlight.h>
#include <linux/folio_batch.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/mm.h>
#include <linux/platform_device.h>
#include <linux/rcupdate.h>
#include <linux/reboot.h>
#include <linux/seq_file.h>
#include <linux/swap.h>
#include <stela/linux.h>
#include <unsupported.h>

/* Pages behind shmem files and shmem-backed GEM objects, which swap and
 * nouveau's TTM objects never need: TTM allocates pages from its own pool and
 * Stela never reclaims them. */
STELA_UNSUPPORTED(struct folio *, shmem_read_folio_gfp, struct address_space *, pgoff_t, gfp_t)
STELA_UNSUPPORTED(struct page *, shmem_read_mapping_page_gfp, struct address_space *, pgoff_t,
		  gfp_t)
STELA_UNSUPPORTED(void, shmem_truncate_range, struct inode *, loff_t, uoff_t)
struct swap_iocb;
STELA_UNSUPPORTED(int, shmem_writeout, struct folio *, struct swap_iocb **, struct list_head *)
STELA_UNSUPPORTED(bool, folio_clear_dirty_for_io, struct folio *)
/* Transparent huge mappings of shmem-backed objects. */
STELA_UNSUPPORTED(unsigned long, mm_get_unmapped_area, struct file *, unsigned long,
		  unsigned long, unsigned long, unsigned long)
/* debugfs's seq_file operations, which no file opens: Stela has no debugfs. */
STELA_UNSUPPORTED(int, single_open, struct file *, int (*)(struct seq_file *, void *), void *)
STELA_UNSUPPORTED(int, single_release, struct inode *, struct file *)
STELA_UNSUPPORTED(ssize_t, seq_read, struct file *, char __user *, size_t, loff_t *)
STELA_UNSUPPORTED(loff_t, seq_lseek, struct file *, loff_t, int)
/* Platform devices, of which only HDMI audio codecs reach DRM. */
STELA_UNSUPPORTED(void, platform_device_unregister, struct platform_device *)
/* System RAM lookups, which amdgpu makes only for GFX9 APUs' coherence flags. */
STELA_UNSUPPORTED(int, page_is_ram, unsigned long)
/* An emergency restart, which amdgpu requests only after a fatal RAS error on
 * a Vega20 with early firmware. */
STELA_UNSUPPORTED(void, emergency_restart, void)
/* Backlights of amdgpu's legacy DCE encoders; DCN parts drive their displays
 * through DC. */
STELA_UNSUPPORTED(struct backlight_device *, backlight_device_register, const char *,
		  struct device *, void *, const struct backlight_ops *,
		  const struct backlight_properties *)
STELA_UNSUPPORTED(void, backlight_device_unregister, struct backlight_device *)
/* Interrupt domains (CONFIG_IRQ_DOMAIN), which amdgpu creates only for its
 * VI-era and IH 6.1 interrupt handlers and maps only for ACP and ISP. */
STELA_UNSUPPORTED(struct irq_domain *, irq_domain_create_linear, struct fwnode_handle *,
		  unsigned int, const struct irq_domain_ops *, void *)
STELA_UNSUPPORTED(void, irq_domain_remove, struct irq_domain *)
STELA_UNSUPPORTED(unsigned int, irq_create_mapping, struct irq_domain *, irq_hw_number_t)
STELA_UNSUPPORTED(int, generic_handle_domain_irq, struct irq_domain *, irq_hw_number_t)
STELA_UNSUPPORTED(void, handle_simple_irq, struct irq_desc *)
STELA_UNSUPPORTED(void, irq_set_chip_and_handler_name, unsigned int, const struct irq_chip *,
		  irq_flow_handler_t, const char *)
