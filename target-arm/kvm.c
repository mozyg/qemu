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

    memcpy(regs.regs, env->regs, sizeof(uint32_t) * 16);
    /* TODO: Figure out if offset is in beginning or end of banked_spsr */
    memcpy(regs.banked_spsr,
	   &env->banked_spsr[1],
	   sizeof(uint32_t) * 5);
    memcpy(regs.banked_r13, env->banked_r13, sizeof(uint32_t) * 6);
    memcpy(regs.banked_r14, env->banked_r14, sizeof(uint32_t) * 6);
    memcpy(regs.usr_regs, env->usr_regs, sizeof(uint32_t) * 5);
    memcpy(regs.fiq_regs, env->fiq_regs, sizeof(uint32_t) * 5);
    regs.cpsr = cpsr_read(env);
    regs.spsr = env->spsr;

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

    memcpy(env->regs, regs.regs, sizeof(uint32_t) * 16);
    /* TODO: Figure out if offset is in beginning or end of banked_spsr */
    memcpy(&env->banked_spsr[1],
	   regs.banked_spsr,
	   sizeof(uint32_t) * 5);
    memcpy(env->banked_r13, regs.banked_r13, sizeof(uint32_t) * 6);
    memcpy(env->banked_r14, regs.banked_r14, sizeof(uint32_t) * 6);
    memcpy(env->usr_regs, regs.usr_regs, sizeof(uint32_t) * 5);
    memcpy(env->fiq_regs, regs.fiq_regs, sizeof(uint32_t) * 5);
    cpsr_write(env, regs.cpsr, 0xFFFFFFFF);
    env->spsr = regs.spsr;

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

