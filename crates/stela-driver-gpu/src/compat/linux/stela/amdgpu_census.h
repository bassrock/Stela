/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Kernel services amdgpu calls that Linux's headers declare only under
 * options Stela's Kconfig leaves unset: the interrupt domain of its VI-era
 * and IH 6.1 interrupt handlers (CONFIG_IRQ_DOMAIN) and the page-to-node
 * lookup of GMC 9's APU coherence flags (a memory model). Declared here so
 * the amdgpu census counts each as an open import rather than failing to
 * compile; nothing defines them. */
#ifndef STELA_AMDGPU_CENSUS_H
#define STELA_AMDGPU_CENSUS_H

#include <linux/irqdomain.h>
#include <linux/types.h>

struct irq_domain *irq_domain_create_linear(struct fwnode_handle *fwnode,
					    unsigned int size,
					    const struct irq_domain_ops *ops,
					    void *host_data);
void irq_domain_remove(struct irq_domain *domain);
unsigned int irq_create_mapping(struct irq_domain *domain,
				irq_hw_number_t hwirq);
int generic_handle_domain_irq(struct irq_domain *domain,
			      irq_hw_number_t hwirq);
int pfn_to_nid(unsigned long pfn);

#endif
