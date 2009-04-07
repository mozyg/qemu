/*
 * PowerPC implementation of KVM hooks
 *
 * Copyright IBM Corp. 2007
 *
 * Authors:
 *  Jerone Young <jyoung5@us.ibm.com>
 *  Christian Ehrhardt <ehrhardt@linux.vnet.ibm.com>
 *  Hollis Blanchard <hollisb@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/kvm.h>

#include "qemu-common.h"
#include "qemu-timer.h"
#include "sysemu.h"
#include "kvm.h"
#include "cpu.h"
#include "device_tree.h"

int kvm_arch_init(KVMState *s, int smp_cpus)
{
    return 0;
}

int kvm_arch_init_vcpu(CPUState *cenv)
{
    return 0;
}

int kvm_arch_put_registers(CPUState *env)
{
    struct kvm_regs regs;
    int ret;

    ret = kvm_vcpu_ioctl(env, KVM_GET_REGS, &regs);
    if (ret < 0)
        return ret;

    ret = kvm_vcpu_ioctl(env, KVM_SET_REGS, &regs);
    if (ret < 0)
        return ret;

    return ret;
}

int kvm_arch_get_registers(CPUState *env)
{
    struct kvm_regs regs;
    uint32_t ret;

    ret = kvm_vcpu_ioctl(env, KVM_GET_REGS, &regs);
    if (ret < 0)
        return ret;

    return 0;
}

int kvm_arch_pre_run(CPUState *env, struct kvm_run *run)
{
    return 0;
}

int kvm_arch_post_run(CPUState *env, struct kvm_run *run)
{
    return 0;
}

int kvm_arch_handle_exit(CPUState *env, struct kvm_run *run)
{
    int ret = 0;

    return ret;
}

