// SPDX-License-Identifier: GPL-2.0
/*
 * Allwinner PCI Express plat driver
 *
 * Copyright(c) 2020 - 2024 Allwinner Technology Co.,Ltd. All rights reserved.
 *
 * pcie-sun55i-plat.c:	chenhuaqiang <chenhuaqiang@allwinnertech.com>
 */


#include "pci.h"
#include "pcie-sun55i.h"
#include <linux/types.h>
#include <linux/delay.h>
#include <asm/arch/clock.h>
#include <power/regulator.h>
#include <dm.h>

/* Indexed by PCI_EXP_LNKCAP_SLS, PCI_EXP_LNKSTA_CLS */
const unsigned char pcie_link_speed[] = {
	PCI_SPEED_UNKNOWN,		/* 0 */
	PCIE_SPEED_2_5GT,		/* 1 */
	PCIE_SPEED_5_0GT,		/* 2 */
	PCIE_SPEED_8_0GT,		/* 3 */
	PCIE_SPEED_16_0GT,		/* 4 */
	PCIE_SPEED_32_0GT,		/* 5 */
};

int sun55i_pcie_cfg_write(void __iomem *addr, int size, ulong val)
{
	if ((uintptr_t)addr & (size - 1))
		return PCIBIOS_BAD_REGISTER_NUMBER;

	if (size == 4)
		writel(val, addr);
	else if (size == 2)
		writew(val, addr);
	else if (size == 1)
		writeb(val, addr);
	else
		return PCIBIOS_BAD_REGISTER_NUMBER;

	return PCIBIOS_SUCCESSFUL;
}

int sun55i_pcie_cfg_read(void __iomem *addr, int size, ulong *val)
{
    
    if ((uintptr_t)addr & (size - 1)) {
        *val = 0;
        printf("CFG_READ: Bad alignment\n");
        return PCIBIOS_BAD_REGISTER_NUMBER;
    }

    if (size == 4) {
        *val = readl(addr);
    } else if (size == 2) {
        *val = readw(addr);
    } else if (size == 1) {
        *val = readb(addr);
    } else {
        *val = 0;
        printf("CFG_READ: Bad size\n");
        return PCIBIOS_BAD_REGISTER_NUMBER;
    }

    return PCIBIOS_SUCCESSFUL;
}

void sun55i_pcie_writel(u32 val, struct sun55i_pcie *pcie, u32 offset)
{
	writel(val, pcie->app_base + offset);
}

u32 sun55i_pcie_readl(struct sun55i_pcie *pcie, u32 offset)
{
	return readl(pcie->app_base + offset);
}

static void sun55i_pcie_write_dbi(struct sun55i_pcie *pci, u32 reg, size_t size, u32 val)
{
	int ret;

	ret = sun55i_pcie_cfg_write(pci->dbi_base + reg, size, val);
	if (ret)
		printf("Write DBI address failed\n");
}

static ulong sun55i_pcie_read_dbi(struct sun55i_pcie *pci, u32 reg, size_t size)
{
	int ret;
	ulong val;

	ret = sun55i_pcie_cfg_read(pci->dbi_base + reg, size, &val);
	if (ret)
		printf("Read DBI address failed\n");

	return val;
}

void sun55i_pcie_writel_dbi(struct sun55i_pcie *pci, u32 reg, u32 val)
{
	sun55i_pcie_write_dbi(pci, reg, 0x4, val);
}

u32 sun55i_pcie_readl_dbi(struct sun55i_pcie *pci, u32 reg)
{
	return sun55i_pcie_read_dbi(pci, reg, 0x4);
}

void sun55i_pcie_writew_dbi(struct sun55i_pcie *pci, u32 reg, u16 val)
{
	sun55i_pcie_write_dbi(pci, reg, 0x2, val);
}

u16 sun55i_pcie_readw_dbi(struct sun55i_pcie *pci, u32 reg)
{
	return sun55i_pcie_read_dbi(pci, reg, 0x2);
}

void sun55i_pcie_writeb_dbi(struct sun55i_pcie *pci, u32 reg, u8 val)
{
	sun55i_pcie_write_dbi(pci, reg, 0x1, val);
}

u8 sun55i_pcie_readb_dbi(struct sun55i_pcie *pci, u32 reg)
{
	return sun55i_pcie_read_dbi(pci, reg, 0x1);
}

void sun55i_pcie_dbi_ro_wr_en(struct sun55i_pcie *pci)
{
	u32 val;

	val = sun55i_pcie_readl_dbi(pci, PCIE_MISC_CONTROL_1_CFG);
	val |= (0x1 << 0);
	sun55i_pcie_writel_dbi(pci, PCIE_MISC_CONTROL_1_CFG, val);
}

void sun55i_pcie_dbi_ro_wr_dis(struct sun55i_pcie *pci)
{
	u32 val;

	val = sun55i_pcie_readl_dbi(pci, PCIE_MISC_CONTROL_1_CFG);
	val &= ~(0x1 << 0);
	sun55i_pcie_writel_dbi(pci, PCIE_MISC_CONTROL_1_CFG, val);
}

void sun55i_pcie_plat_ltssm_enable(struct sun55i_pcie *pcie)
{
	u32 val;

	val = sun55i_pcie_readl(pcie, PCIE_LTSSM_CTRL);
	val |= PCIE_LINK_TRAINING;
	sun55i_pcie_writel(val, pcie, PCIE_LTSSM_CTRL);
}

void sun55i_pcie_plat_ltssm_disable(struct sun55i_pcie *pcie)
{
	u32 val;

	val = sun55i_pcie_readl(pcie, PCIE_LTSSM_CTRL);
	val &= ~PCIE_LINK_TRAINING;
	sun55i_pcie_writel(val, pcie, PCIE_LTSSM_CTRL);
}

static u8 __sun55i_pcie_find_next_cap(struct sun55i_pcie *pci, u8 cap_ptr,
						u8 cap)
{
	u8 cap_id, next_cap_ptr;
	u16 reg;

	if (!cap_ptr)
		return 0;

	reg = sun55i_pcie_readw_dbi(pci, cap_ptr);
	cap_id = (reg & CAP_ID_MASK);

	if (cap_id > PCI_CAP_ID_MAX)
		return 0;

	if (cap_id == cap)
		return cap_ptr;

	next_cap_ptr = (reg & NEXT_CAP_PTR_MASK) >> 8;
	return __sun55i_pcie_find_next_cap(pci, next_cap_ptr, cap);
}

u8 sun55i_pcie_plat_find_capability(struct sun55i_pcie *pci, u8 cap)
{
	u8 next_cap_ptr;
	u16 reg;

	reg = sun55i_pcie_readw_dbi(pci, PCI_CAPABILITY_LIST);
	next_cap_ptr = (reg & CAP_ID_MASK);

	return __sun55i_pcie_find_next_cap(pci, next_cap_ptr, cap);
}

static void sun55i_pcie_plat_set_link_cap(struct sun55i_pcie *pci, u32 link_gen)
{
	u32 cap, ctrl2, link_speed = 0;

	u8 offset = sun55i_pcie_plat_find_capability(pci, PCI_CAP_ID_EXP);

	cap = sun55i_pcie_readl_dbi(pci, offset + PCI_EXP_LNKCAP);
	ctrl2 = sun55i_pcie_readl_dbi(pci, offset + PCI_EXP_LNKCTL2);
	ctrl2 &= ~PCI_EXP_LNKCTL2_TLS;

	switch (pcie_link_speed[link_gen]) {
	case PCIE_SPEED_2_5GT:
		link_speed = PCI_EXP_LNKCTL2_TLS_2_5GT;
		break;
	case PCIE_SPEED_5_0GT:
		link_speed = PCI_EXP_LNKCTL2_TLS_5_0GT;
		break;
	case PCIE_SPEED_8_0GT:
		link_speed = PCI_EXP_LNKCTL2_TLS_8_0GT;
		break;
	case PCIE_SPEED_16_0GT:
		link_speed = PCI_EXP_LNKCTL2_TLS_16_0GT;
		break;
	default:
		/* Use hardware capability */
		// link_speed = FIELD_GET(PCI_EXP_LNKCAP_SLS, cap);
		// ctrl2 &= ~PCI_EXP_LNKCTL2_HASD;
		break;
	}

	sun55i_pcie_writel_dbi(pci, offset + PCI_EXP_LNKCTL2, ctrl2 | link_speed);

	cap &= ~((u32)PCI_EXP_LNKCAP_SLS);
	sun55i_pcie_writel_dbi(pci, offset + PCI_EXP_LNKCAP, cap | link_speed);
}

void sun55i_pcie_plat_set_rate(struct sun55i_pcie *pci)
{
	u32 val;

	sun55i_pcie_plat_set_link_cap(pci, pci->link_gen);
	/* set the number of lanes */
	val = sun55i_pcie_readl_dbi(pci, PCIE_PORT_LINK_CONTROL);
	val &= ~PORT_LINK_MODE_MASK;
	switch (pci->lanes) {
	case 1:
		val |= PORT_LINK_MODE_1_LANES;
		break;
	case 2:
		val |= PORT_LINK_MODE_2_LANES;
		break;
	case 4:
		val |= PORT_LINK_MODE_4_LANES;
		break;
	default:
		printf("num-lanes %u: invalid value\n", pci->lanes);
		return;
	}
	sun55i_pcie_writel_dbi(pci, PCIE_PORT_LINK_CONTROL, val);

	/* set link width speed control register */
	val = sun55i_pcie_readl_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL);
	val &= ~PORT_LOGIC_LINK_WIDTH_MASK;
	switch (pci->lanes) {
	case 1:
		val |= PORT_LOGIC_LINK_WIDTH_1_LANES;
		break;
	case 2:
		val |= PORT_LOGIC_LINK_WIDTH_2_LANES;
		break;
	case 4:
		val |= PORT_LOGIC_LINK_WIDTH_4_LANES;
		break;
	}
	sun55i_pcie_writel_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL, val);
}

static int sun55i_pcie_plat_init_port(struct udevice *dev)
{
	struct sun55i_pcie *pci = dev_get_priv(dev);
	int ret;

	if (dm_gpio_is_valid(&pci->wake_gpio)) {
        ret = dm_gpio_set_value(&pci->wake_gpio, 1);
        if (ret) {
            printf("PCIe: Failed to set wake GPIO: %d\n", ret);
            return ret;
        }
    }

	ret = dm_gpio_set_value(&pci->switch_gpio, 1);
	if (ret) {
		printf("PCIe: Failed to set switch GPIO: %d\n", ret);
		return ret;
	}

	ret = regulator_set_enable(pci->slot_3v3, true);
	if (ret && ret != -EALREADY) {
		printf("PCIe: Failed to enable 3.3V slot supply: %d\n", ret);
		return ret;
	}

	mdelay(50);

	ret = clk_enable(&pci->pcie_aux);
	if (ret) {
		printf("PCIe: Failed to enable bus clock: %d\n", ret);
		goto err_disable_slot_supply;
	}

	if (pci->drvdata && pci->drvdata->need_pcie_rst) {
		ret = reset_deassert(&pci->pcie_rst);
		if (ret) {
			printf("PCIe: Failed to deassert internal reset: %d\n", ret);
			goto err_disable_clk;
		}
	}

	ret = generic_phy_init(&pci->phy);
	if (ret) {
		printf("PCIe: Failed to init phy: %d\n", ret);
		goto err_assert_reset;
	}
	ret = generic_phy_power_on(&pci->phy);
	if (ret) {
		printf("PCIe: Failed to power on phy: %d\n", ret);
		goto err_assert_reset;
	}

	printf("PCIe: Toggling external device reset (PERST#)...\n");
	ret = dm_gpio_set_value(&pci->rst_gpio, 0);
	if (ret) {
		printf("PCIe: Failed to assert external reset: %d\n", ret);
		goto err_power_off_phy;
	}

	mdelay(100);

	ret = dm_gpio_set_value(&pci->rst_gpio, 1);
	if (ret) {
		printf("PCIe: Failed to deassert external reset: %d\n", ret);
		goto err_power_off_phy;
	}

	mdelay(40);

	printf("PCIe: Hardware power-on sequence successful.\n");
	return 0;

err_power_off_phy:
	generic_phy_power_off(&pci->phy);
err_assert_reset:
	if (pci->drvdata && pci->drvdata->need_pcie_rst)
		reset_assert(&pci->pcie_rst);
err_disable_clk:
	clk_disable(&pci->pcie_aux);
err_disable_slot_supply:
	regulator_set_enable(pci->slot_3v3, false);

	return ret;
}

int sun55i_pcie_plat_hw_init(struct udevice *dev)
{
	struct sun55i_pcie *pci = dev_get_priv(dev);
	int ret;

	printf("PCIe: Acquiring resources...\n");

	ret = dev_read_u32(dev, "num-lanes", &pci->lanes);
	if (ret) {
		printf("PCIe: Failed to parse num-lanes, using default: 1\n");
		pci->lanes = 1;
	}

	ret = dev_read_u32(dev, "max-link-speed", &pci->link_gen);
	if (ret) {
		printf("PCIe: Couldn't parse max-link-speed, using default link speed: Gen2\n");
		pci->link_gen = 2;
	}

	if (pci->lanes != 1 && pci->lanes != 2 && pci->lanes != 4) {
		printf("PCIe: Invalid num-lanes %d, using 1\n", pci->lanes);
		pci->lanes = 1;
	}

	if (pci->link_gen < 1 || pci->link_gen > 3) {
		printf("PCIe: Invalid max-link-speed %d, using 2\n", pci->link_gen);
		pci->link_gen = 2;
	}

	ret = gpio_request_by_name(dev, "switch-sel-gpios", 0, &pci->switch_gpio, GPIOD_IS_OUT);
	if (ret) {
		printf("PCIe: Failed to get switch-sel GPIO: %d\n", ret);
		return ret;
	}

	ret = gpio_request_by_name(dev, "reset-gpios", 0, &pci->rst_gpio,
                               GPIOD_IS_OUT);
    if (ret) {
        printf("PCIe: Failed to get reset-gpios: %d\n", ret);
        return ret;
    }

    ret = gpio_request_by_name(dev, "wake-gpios", 0, &pci->wake_gpio,
                               GPIOD_IS_OUT);
    if (ret) {
        printf("PCIe: Warning: Failed to get wake-gpios: %d\n", ret);
    }

	ret = device_get_supply_regulator(dev, "slot-3v3-supply", &pci->slot_3v3);
	if (ret) {
		printf("PCIe: Failed to get 3.3V slot supply: %d\n", ret);
		return ret;
	}

	ret = clk_get_by_name(dev, "pclk_aux", &pci->pcie_aux);
	if (ret) {
		printf("PCIe: Failed to get bus clock: %d\n", ret);
		return ret;
	}

	pci->drvdata = (const struct sun55i_pcie_of_data *)dev_get_driver_data(dev);
	if (pci->drvdata && pci->drvdata->need_pcie_rst) {
		ret = reset_get_by_index(dev, 0, &pci->pcie_rst);
		if (ret) {
			printf("PCIe: Failed to get reset controller: %d\n", ret);
			return ret;
		}
	}

	ret = generic_phy_get_by_index(dev, 0, &pci->phy);
	if (ret) {
		printf("PCIe: Failed to get phy: %d\n", ret);
		return ret;
	}

	printf("PCIe: All resources acquired. Starting power-on sequence...\n");

	return sun55i_pcie_plat_init_port(dev);

}



