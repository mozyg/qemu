/*
 * Qemu PowerPC 440 Bamboo board emulation
 *
 * Copyright 2007 IBM Corporation.
 * Authors:
 * 	Jerone Young <jyoung5@us.ibm.com>
 * 	Christian Ehrhardt <ehrhardt@linux.vnet.ibm.com>
 * 	Hollis Blanchard <hollisb@us.ibm.com>
 *
 * This work is licensed under the GNU GPL license version 2 or later.
 *
 */

#include "config.h"
#include "qemu-common.h"
#include "net.h"
#include "hw.h"
#include "pci.h"
#include "boards.h"
#include "sysemu.h"
#include "ppc440.h"
#include "kvm.h"
#include "kvm_ppc.h"
#include "device_tree.h"
#include "loader.h"
#include "elf.h"

#define BINARY_DEVICE_TREE_FILE "bamboo.dtb"

static void *bamboo_load_device_tree(target_phys_addr_t addr,
                                     uint32_t ramsize,
                                     target_phys_addr_t initrd_base,
                                     target_phys_addr_t initrd_size,
                                     const char *kernel_cmdline)
{
    void *fdt = NULL;
#ifdef CONFIG_FDT
    uint32_t mem_reg_property[] = { 0, 0, ramsize };
    char *filename;
    int fdt_size;
    int ret;

    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, BINARY_DEVICE_TREE_FILE);
    if (!filename) {
        goto out;
    }
    fdt = load_device_tree(filename, &fdt_size);
    qemu_free(filename);
    if (fdt == NULL) {
        goto out;
    }

    /* Manipulate device tree in memory. */

    ret = qemu_devtree_setprop(fdt, "/memory", "reg", mem_reg_property,
                               sizeof(mem_reg_property));
    if (ret < 0)
        fprintf(stderr, "couldn't set /memory/reg\n");

    ret = qemu_devtree_setprop_cell(fdt, "/chosen", "linux,initrd-start",
                                    initrd_base);
    if (ret < 0)
        fprintf(stderr, "couldn't set /chosen/linux,initrd-start\n");

    ret = qemu_devtree_setprop_cell(fdt, "/chosen", "linux,initrd-end",
                                    (initrd_base + initrd_size));
    if (ret < 0)
        fprintf(stderr, "couldn't set /chosen/linux,initrd-end\n");

    ret = qemu_devtree_setprop_string(fdt, "/chosen", "bootargs",
                                      kernel_cmdline);
    if (ret < 0)
        fprintf(stderr, "couldn't set /chosen/bootargs\n");

    if (kvm_enabled())
        kvmppc_fdt_update(fdt);

    cpu_physical_memory_write (addr, (void *)fdt, fdt_size);

out:
#endif

    return fdt;
}

static void bamboo_init(ram_addr_t ram_size,
                        const char *boot_device,
                        const char *kernel_filename,
                        const char *kernel_cmdline,
                        const char *initrd_filename,
                        const char *cpu_model)
{
    unsigned int pci_irq_nrs[4] = { 28, 27, 26, 25 };
    PCIBus *pcibus;
    CPUState *env;
    uint64_t elf_entry;
    uint64_t elf_lowaddr;
    target_phys_addr_t entry = 0;
    target_phys_addr_t loadaddr = 0;
    target_long kernel_size = 0;
    target_ulong initrd_base = 0;
    target_long initrd_size = 0;
    target_ulong dt_base = 0;
    void *fdt;
    int i;

    /* Setup CPU. */
    env = ppc440ep_init(&ram_size, &pcibus, pci_irq_nrs, 1, cpu_model);

    if (pcibus) {
        /* Register network interfaces. */
        for (i = 0; i < nb_nics; i++) {
            /* There are no PCI NICs on the Bamboo board, but there are
             * PCI slots, so we can pick whatever default model we want. */
            pci_nic_init_nofail(&nd_table[i], "e1000", NULL);
        }
    }

    /* Load kernel. */
    if (kernel_filename) {
        kernel_size = load_uimage(kernel_filename, &entry, &loadaddr, NULL);
        if (kernel_size < 0) {
            kernel_size = load_elf(kernel_filename, 0, &elf_entry, &elf_lowaddr,
                                   NULL, 1, ELF_MACHINE, 0);
            entry = elf_entry;
            loadaddr = elf_lowaddr;
        }
        /* XXX try again as binary */
        if (kernel_size < 0) {
            fprintf(stderr, "qemu: could not load kernel '%s'\n",
                    kernel_filename);
            exit(1);
        }
    }

    /* Load initrd. */
    if (initrd_filename) {
        initrd_base = kernel_size + loadaddr;
        initrd_size = load_image_targphys(initrd_filename, initrd_base,
                                          ram_size - initrd_base);

        if (initrd_size < 0) {
            fprintf(stderr, "qemu: could not load initial ram disk '%s'\n",
                    initrd_filename);
            exit(1);
        }
    }

    /* If we're loading a kernel directly, we must load the device tree too. */
    if (kernel_filename) {
        if (initrd_base)
            dt_base = initrd_base + initrd_size;
        else
            dt_base = kernel_size + loadaddr;

        fdt = bamboo_load_device_tree(dt_base, ram_size,
                                      initrd_base, initrd_size, kernel_cmdline);
        if (fdt == NULL) {
            fprintf(stderr, "couldn't load device tree\n");
            exit(1);
        }

        /* Set initial guest state. */
        env->gpr[1] = (16<<20) - 8;
        env->gpr[3] = dt_base;
        env->nip = entry;
        /* XXX we currently depend on KVM to create some initial TLB entries. */
    }

    if (kvm_enabled())
        kvmppc_init();
}

static QEMUMachine bamboo_machine = {
    .name = "bamboo",
    .desc = "bamboo",
    .init = bamboo_init,
};

static void bamboo_machine_init(void)
{
    qemu_register_machine(&bamboo_machine);
}

machine_init(bamboo_machine_init);
