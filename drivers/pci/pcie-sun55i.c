// SPDX-License-Identifier: GPL-2.0+
/*
 * sun55i DesignWare based PCIe host controller driver
 *
 * Copyright (c) 2021 sun55i, Inc.
 */


#include <dm.h>
#include <syscon.h>
#include <asm/io.h>
#include <asm/arch-sunxi/clock.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include "pcie-sun55i.h"

DECLARE_GLOBAL_DATA_PTR;

#define sun55i_pcie_DBG			0

#define __pcie_dev_print_emit(fmt, ...) \
({ \
	printf(fmt, ##__VA_ARGS__); \
})

#ifdef dev_err
#undef dev_err
#define dev_err(dev, fmt, ...) \
({ \
	if (dev) \
		__pcie_dev_print_emit("%s: " fmt, dev->name, \
				##__VA_ARGS__); \
})
#endif

#ifdef dev_info
#undef dev_info
#define dev_info dev_err
#endif

#ifdef DEBUG
#define dev_dbg dev_err
#else
#define dev_dbg(dev, fmt, ...)					\
({								\
	if (0)							\
		__dev_printk(7, dev, fmt, ##__VA_ARGS__);	\
})
#endif



static int sun55i_pcie_addr_valid(pci_dev_t d, int first_busno)
{
	if ((PCI_BUS(d) == first_busno) && (PCI_DEV(d) > 0))
		return 0;
	if ((PCI_BUS(d) == first_busno + 1) && (PCI_DEV(d) > 0))
		return 0;

	return 1;
}

static void sun55i_pcie_prog_outbound_atu(struct sun55i_pcie_port *pp, int index, int type,
					u64 cpu_addr, u64 pci_addr, u32 size)
{
	struct sun55i_pcie *pci = to_sun55i_pcie_from_pp(pp);


	sun55i_pcie_writel_dbi(pci, PCIE_ATU_LOWER_BASE_OUTBOUND(index), lower_32_bits(cpu_addr));
	sun55i_pcie_writel_dbi(pci, PCIE_ATU_UPPER_BASE_OUTBOUND(index), upper_32_bits(cpu_addr));
	sun55i_pcie_writel_dbi(pci, PCIE_ATU_LIMIT_OUTBOUND(index), lower_32_bits(cpu_addr + size - 1));
	sun55i_pcie_writel_dbi(pci, PCIE_ATU_LOWER_TARGET_OUTBOUND(index), lower_32_bits(pci_addr));
	sun55i_pcie_writel_dbi(pci, PCIE_ATU_UPPER_TARGET_OUTBOUND(index), upper_32_bits(pci_addr));
	sun55i_pcie_writel_dbi(pci, PCIE_ATU_CR1_OUTBOUND(index), type);
	sun55i_pcie_writel_dbi(pci, PCIE_ATU_CR2_OUTBOUND(index), PCIE_ATU_ENABLE);
}

static int sun55i_pcie_rd_other_conf(struct sun55i_pcie_port *pp, pci_dev_t d, int where, int size, ulong *val)
{
	int ret = PCIBIOS_SUCCESSFUL, type;
	u64 busdev;
	u64 atu_cpu_addr = pp->cfg0_base;

	if (pp->cpu_pcie_addr_quirk)
		atu_cpu_addr -= PCIE_CPU_BASE;

	busdev = PCIE_ATU_BUS(PCI_BUS(d)) | PCIE_ATU_DEV(PCI_DEV(d)) | PCIE_ATU_FUNC(PCI_FUNC(d));

	if (PCI_BUS(d) != 0)
		type = PCIE_ATU_TYPE_CFG0;
	else
		type = PCIE_ATU_TYPE_CFG1;

	sun55i_pcie_prog_outbound_atu(pp, PCIE_ATU_INDEX0, type, atu_cpu_addr, busdev, pp->cfg0_size);

	ret = sun55i_pcie_cfg_read(pp->va_cfg0_base + where, size, val);

	return ret;
}

static int sun55i_pcie_wr_other_conf(struct sun55i_pcie_port *pp, pci_dev_t d, int where, int size, ulong val)
{
	int ret = PCIBIOS_SUCCESSFUL, type;
	u64 busdev;
	u64 atu_cpu_addr = pp->cfg0_base; 

	if (pp->cpu_pcie_addr_quirk)
		atu_cpu_addr -= PCIE_CPU_BASE; 

	busdev = PCIE_ATU_BUS(PCI_BUS(d)) | PCIE_ATU_DEV(PCI_DEV(d)) | PCIE_ATU_FUNC(PCI_FUNC(d));

	if (PCI_BUS(d) != 0)
		type = PCIE_ATU_TYPE_CFG0;
	else
		type = PCIE_ATU_TYPE_CFG1;

	sun55i_pcie_prog_outbound_atu(pp, PCIE_ATU_INDEX0, type, atu_cpu_addr, busdev, pp->cfg0_size);

	ret = sun55i_pcie_cfg_write(pp->va_cfg0_base + where, size, val);

	return ret;
}

static int sun55i_pcie_host_rd_own_conf(struct sun55i_pcie_port *pp, int where, int size, ulong *val)
{
	int ret;

	ret = sun55i_pcie_cfg_read(pp->dbi_base + where, size, val);

	return ret;
}

static int sun55i_pcie_host_wr_own_conf(struct sun55i_pcie_port *pp, int where, int size, ulong val)
{
	int ret;

	ret = sun55i_pcie_cfg_write(pp->dbi_base + where, size, val);

	return ret;
}

static int sun55i_pcie_read_config(const struct udevice *bus, pci_dev_t bdf,
				 uint offset, ulong *value,
				 enum pci_size_t size)
{
	struct sun55i_pcie *pcie = dev_get_priv(bus);
	int ret, size_len = 4;

	if (!sun55i_pcie_addr_valid(bdf, pcie->first_busno)) {
		debug("- out of range\n");
		*value = pci_get_ff(size);
		return 0;
	}

	if (size == PCI_SIZE_8)
		size_len = 1;
	else if (size == PCI_SIZE_16)
		size_len = 2;
	else if (size == PCI_SIZE_32)
		size_len = 4;

	if (PCI_BUS(bdf) != pcie->first_busno)
               ret = sun55i_pcie_rd_other_conf(&pcie->pcie_port, bdf, offset, size_len, value);
        else
               ret = sun55i_pcie_host_rd_own_conf(&pcie->pcie_port, offset, size_len, value);

	return ret;
}

static int sun55i_pcie_write_config(struct udevice *bus, pci_dev_t bdf,
				 uint offset, ulong value,
				 enum pci_size_t size)
{
	struct sun55i_pcie *pcie = dev_get_priv(bus);
	int ret, size_len = 4;

	if (!sun55i_pcie_addr_valid(bdf, pcie->first_busno)) {
		debug("- out of range\n");
		return 0;
	}

	if (size == PCI_SIZE_8)
		size_len = 1;
	else if (size == PCI_SIZE_16)
	size_len = 2;
	else if (size == PCI_SIZE_32)
		size_len = 4;

	if (PCI_BUS(bdf) != pcie->first_busno)
		ret = sun55i_pcie_wr_other_conf(&pcie->pcie_port, bdf, offset, size_len, value);
	else
		ret = sun55i_pcie_host_wr_own_conf(&pcie->pcie_port, offset, size_len, value);

	return ret;
}

static void sun55i_pcie_host_setup_rc(struct sun55i_pcie_port *pp)
{
	ulong val, i;
	phys_addr_t mem_base;
	phys_addr_t io_base;
	struct sun55i_pcie *pci = to_sun55i_pcie_from_pp(pp);

	sun55i_pcie_plat_set_rate(pci);

	sun55i_pcie_writel_dbi(pci, PCI_BASE_ADDRESS_0, 0x4);
	sun55i_pcie_writel_dbi(pci, PCI_BASE_ADDRESS_1, 0x0);

	val = sun55i_pcie_readl_dbi(pci, PCI_INTERRUPT_LINE);
	val &= PCIE_INTERRUPT_LINE_MASK;
	val |= PCIE_INTERRUPT_LINE_ENABLE;
	sun55i_pcie_writel_dbi(pci, PCI_INTERRUPT_LINE, val);

	val = sun55i_pcie_readl_dbi(pci, PCI_PRIMARY_BUS);
	val &= 0xff000000;
	val |= 0x00ff0100;
	sun55i_pcie_writel_dbi(pci, PCI_PRIMARY_BUS, val);

	val = sun55i_pcie_readl_dbi(pci, PCI_COMMAND);

	val &= PCIE_HIGH16_MASK;
	val |= PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
		PCI_COMMAND_MASTER | PCI_COMMAND_SERR;

	sun55i_pcie_writel_dbi(pci, PCI_COMMAND, val);

	if (IS_ENABLED(CONFIG_PCI_MSI) && !pp->has_its) {
		for (i = 0; i < 8; i++) {
			sun55i_pcie_host_wr_own_conf(pp, PCIE_MSI_INTR_ENABLE(i), 4, ~0);
		}
	}

	if (pp->cpu_pcie_addr_quirk) {
		mem_base = pp->mem_base - PCIE_CPU_BASE;
		io_base = pp->io_base - PCIE_CPU_BASE;
	} else {
		mem_base = pp->mem_base;
		io_base = pp->io_base;
	}

	sun55i_pcie_prog_outbound_atu(pp, PCIE_ATU_INDEX1, PCIE_ATU_TYPE_MEM,
					  mem_base, pp->mem_bus_addr, pp->mem_size);

	sun55i_pcie_prog_outbound_atu(pp, PCIE_ATU_INDEX2, PCIE_ATU_TYPE_IO,
					  io_base, pp->io_bus_addr, pp->io_size);

	sun55i_pcie_host_wr_own_conf(pp, PCI_BASE_ADDRESS_0, 4, 0);

	sun55i_pcie_dbi_ro_wr_en(pci);

	sun55i_pcie_host_wr_own_conf(pp, PCI_CLASS_DEVICE, 2, PCI_CLASS_BRIDGE_PCI);

	sun55i_pcie_dbi_ro_wr_dis(pci);

	sun55i_pcie_host_rd_own_conf(pp, PCIE_LINK_WIDTH_SPEED_CONTROL, 4, &val);
	val |= PORT_LOGIC_SPEED_CHANGE;
	sun55i_pcie_host_wr_own_conf(pp, PCIE_LINK_WIDTH_SPEED_CONTROL, 4, val);
}

static int sun55i_pcie_host_link_up_status(struct sun55i_pcie_port *pp)
{
	u32 val;
	int ret;
	struct sun55i_pcie *pcie = to_sun55i_pcie_from_pp(pp);

	val = sun55i_pcie_readl(pcie, PCIE_LINK_STAT);

	if ((val & RDLH_LINK_UP) && (val & SMLH_LINK_UP))
		ret = 1;
	else
		ret = 0;

    debug("  Link Status: 0x%08x\n", val);
    debug("  RDLH_LINK_UP: %d\n", !!(val & RDLH_LINK_UP));
    debug("  SMLH_LINK_UP: %d\n", !!(val & SMLH_LINK_UP));
    debug("  LINK_SPEED: %d\n", (val >> 16) & 0xF);
    printf("  LINK_WIDTH: %d\n", (val >> 20) & 0x3F);

	return ret;
}

static int sun55i_pcie_host_link_up(struct sun55i_pcie_port *pp)
{
	 return sun55i_pcie_host_link_up_status(pp);
}

static int sun55i_pcie_host_wait_for_link(struct sun55i_pcie_port *pp)
{
	int retries;

	for (retries = 0; retries < LINK_WAIT_MAX_RETRIE; retries++) {
		if (sun55i_pcie_host_link_up(pp)) {
			printf("pcie link up success\n");
			return 0;
		}
		mdelay(1);
	}

	return -ETIMEDOUT;
}

static int sun55i_pcie_host_establish_link(struct sun55i_pcie *pci)
{
	struct sun55i_pcie_port *pp = &pci->pcie_port;

	if (sun55i_pcie_host_link_up(pp)) {
		printf("pcie is already link up\n");
		return 0;
	}

	sun55i_pcie_plat_ltssm_enable(pci);

	return sun55i_pcie_host_wait_for_link(pp);
}

static int sun55i_pcie_host_wait_for_speed_change(struct sun55i_pcie *pci)
{
	u32 tmp;
	unsigned int retries;

	for (retries = 0; retries < LINK_WAIT_MAX_RETRIE; retries++) {
		tmp = sun55i_pcie_readl_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL);
		if (!(tmp & PORT_LOGIC_SPEED_CHANGE))
			return 0;
		mdelay(1);
	}

	printf("Speed change timeout\n");
	return -ETIMEDOUT;
}

static int sun55i_pcie_host_speed_change(struct sun55i_pcie *pci, int gen)
{
    u32 val;
    int ret;
    u8 offset;

    sun55i_pcie_dbi_ro_wr_en(pci);

    offset = sun55i_pcie_plat_find_capability(pci, PCI_CAP_ID_EXP);
    if (!offset) {
        printf("PCIe: Cannot find PCI Express capability\n");
        sun55i_pcie_dbi_ro_wr_dis(pci);
        return -EINVAL;
    }

    val = sun55i_pcie_readl_dbi(pci, LINK_CONTROL2_LINK_STATUS2);
    val &= ~PCI_EXP_LNKCTL2_TLS;
    val |= gen;
    sun55i_pcie_writel_dbi(pci, LINK_CONTROL2_LINK_STATUS2, val);

    val = sun55i_pcie_readl_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL);
    val &= ~PORT_LOGIC_SPEED_CHANGE;
    sun55i_pcie_writel_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL, val);

    val = sun55i_pcie_readl_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL);
    val |= PORT_LOGIC_SPEED_CHANGE;
    sun55i_pcie_writel_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL, val);

    ret = sun55i_pcie_host_wait_for_speed_change(pci);
    if (!ret)
        printf("PCIe: Link active at Gen%d\n", gen);
    else
        printf("PCIe: Link active, but speed change failed (remains Gen1)\n");

    sun55i_pcie_dbi_ro_wr_dis(pci);

    return 0;
}

static void sun55i_pcie_host_init(struct udevice *dev)
{
	struct sun55i_pcie *pci = dev_get_priv(dev);

	sun55i_pcie_plat_ltssm_disable(pci);
    
	sun55i_pcie_host_setup_rc(&pci->pcie_port);

	sun55i_pcie_host_establish_link(pci);

	sun55i_pcie_host_speed_change(pci, pci->link_gen);
}

static int sun55i_pcie_probe(struct udevice *dev)
{
	struct sun55i_pcie *pci = dev_get_priv(dev);
	struct udevice *ctlr = pci_get_controller(dev);
	struct pci_controller *hose = dev_get_uclass_priv(ctlr);
	const struct sun55i_pcie_of_data *data;
	int ret;

	data = (const struct sun55i_pcie_of_data *)dev_get_driver_data(dev);
	if (!data) {
		printf("PCIe: No platform data found\n");
		return -EINVAL;
	}

	ret = sun55i_pcie_plat_hw_init(dev);
	if (ret) {
		printf("PCIe: Hardware init failed with error %d\n", ret);
		return ret;
	}

	pci->first_busno = dev->seq_;
	pci->dev = dev;

	pci->dbi_base = (void __iomem *)phys_to_virt((phys_addr_t)data->dbi_addr);
	pci->app_base = (void __iomem *)((char *)pci->dbi_base + PCIE_USER_DEFINED_REGISTER);

	printf("PCIe: Disabling DBI write protection...\n");
	sun55i_pcie_dbi_ro_wr_en(pci);

	pci_set_region(&hose->regions[0],
			data->io_addr,
			data->io_addr,
			data->io_size,
			PCI_REGION_IO);

	pci_set_region(&hose->regions[1],
			data->mem_addr,
			data->mem_addr,
			data->mem_size,
			PCI_REGION_MEM);

	hose->region_count = 2;

	pci->pcie_port.dbi_base = (void __iomem *)phys_to_virt((phys_addr_t)data->dbi_addr);
	pci->pcie_port.cfg0_base = data->cfg_addr;
	pci->pcie_port.cfg0_size = data->cfg_size;
	pci->pcie_port.io_base   = data->io_addr;
	pci->pcie_port.io_size   = data->io_size;
	pci->pcie_port.mem_base  = data->mem_addr;
	pci->pcie_port.mem_size  = data->mem_size;

	pci->pcie_port.io_bus_addr  = data->io_addr;
	pci->pcie_port.mem_bus_addr = data->mem_addr;

	if (!pci->lanes)
		pci->lanes = data->num_lanes;
	if (!pci->link_gen)
		pci->link_gen = data->max_link_speed;

	pci->pcie_port.cpu_pcie_addr_quirk = true;

	pci->pcie_port.va_cfg0_base = phys_to_virt(pci->pcie_port.cfg0_base);

	printf("PCIe: DBI  region: 0x%08x-0x%08x\n", data->dbi_addr, data->dbi_addr + data->dbi_size);
	printf("PCIe: IO   region: 0x%08x-0x%08x\n", data->io_addr, data->io_addr + data->io_size);
	printf("PCIe: MEM  region: 0x%08x-0x%08x\n", data->mem_addr, data->mem_addr + data->mem_size);
	printf("PCIe: CFG  region: 0x%08x-0x%08x\n", data->cfg_addr, data->cfg_addr + data->cfg_size);
	printf("PCIe: Lanes: %d, Max Speed: Gen%d\n", data->num_lanes, data->max_link_speed);

	sun55i_pcie_host_init(dev);
	
	sun55i_pcie_dbi_ro_wr_dis(pci);

	return 0;
}

static const struct dm_pci_ops sun55i_pcie_ops = {
	.read_config	= sun55i_pcie_read_config,
	.write_config	= sun55i_pcie_write_config,
};

static const struct sun55i_pcie_of_data sun55i_pcie_rc_v210_of_data = {
    .mode = SUN55I_PCIE_RC_TYPE,
	.cpu_pcie_addr_quirk = true,

    .dbi_addr       = 0x04800000,
    .dbi_size       = 0x480000,
    .io_addr        = 0x21000000,
    .io_size        = 0x01000000,
    .mem_addr       = 0x22000000,
    .mem_size       = 0x0e000000,
    .cfg_addr       = 0x20000000,  
    .cfg_size       = 0x01000000,
    .num_lanes      = 1,           /* Default */
    .max_link_speed = 2,
    .num_ib_windows = 8,
    .num_ob_windows = 8,
};

static const struct udevice_id sun55i_pcie_ids[] = {
	{
		.compatible = "allwinner,sun55i-pcie-v210-rc",
		.data = (ulong)&sun55i_pcie_rc_v210_of_data,
	},
	{ }
};

U_BOOT_DRIVER(sun55i_pcie) = {
	.name			= "pcie_dw_sun55i",
	.id			= UCLASS_PCI,
	.of_match		= sun55i_pcie_ids,
	.ops			= &sun55i_pcie_ops,
	.probe			= sun55i_pcie_probe,
	.priv_auto 	= sizeof(struct sun55i_pcie),
};
