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

    memcpy(regs.regs0_7, env->regs, sizeof(uint32_t) * 8);
    memcpy(regs.usr_regs8_12, env->usr_regs, sizeof(uint32_t) * 5);
    memcpy(regs.fiq_regs8_12, env->fiq_regs, sizeof(uint32_t) * 5);
    regs.reg13[MODE_FIQ] = env->banked_r13[5];
    regs.reg13[MODE_IRQ] = env->banked_r13[4];
    regs.reg13[MODE_SUP] = env->banked_r13[1];
    regs.reg13[MODE_ABORT] = env->banked_r13[2];
    regs.reg13[MODE_UNDEF] = env->banked_r13[3];
    regs.reg13[MODE_USER] = env->banked_r13[0];
    regs.reg14[MODE_FIQ] = env->banked_r14[5];
    regs.reg14[MODE_IRQ] = env->banked_r14[4];
    regs.reg14[MODE_SUP] = env->banked_r14[1];
    regs.reg14[MODE_ABORT] = env->banked_r14[2];
    regs.reg14[MODE_UNDEF] = env->banked_r14[3];
    regs.reg14[MODE_USER] = env->banked_r14[0];
    regs.reg15 = env->regs[15];
    regs.cpsr = cpsr_read(env);
    regs.spsr[MODE_FIQ] = env->banked_spsr[5];
    regs.spsr[MODE_IRQ] = env->banked_spsr[4];
    regs.spsr[MODE_SUP] = env->banked_spsr[1];
    regs.spsr[MODE_ABORT] = env->banked_spsr[2];
    regs.spsr[MODE_UNDEF] = env->banked_spsr[3];

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
    memcpy(env->regs, regs.regs0_7, sizeof(uint32_t) * 8);
    memcpy(env->usr_regs, regs.usr_regs8_12, sizeof(uint32_t) * 5);
    memcpy(env->fiq_regs, regs.fiq_regs8_12, sizeof(uint32_t) * 5);
    env->banked_r13[5] = regs.reg13[MODE_FIQ];
    env->banked_r13[4] = regs.reg13[MODE_IRQ];
    env->banked_r13[1] = regs.reg13[MODE_SUP];
    env->banked_r13[2] = regs.reg13[MODE_ABORT];
    env->banked_r13[3] = regs.reg13[MODE_UNDEF];
    env->banked_r13[0] = regs.reg13[MODE_USER];
    env->banked_r14[5] = regs.reg14[MODE_FIQ];
    env->banked_r14[4] = regs.reg14[MODE_IRQ];
    env->banked_r14[1] = regs.reg14[MODE_SUP];
    env->banked_r14[2] = regs.reg14[MODE_ABORT];
    env->banked_r14[3] = regs.reg14[MODE_UNDEF];
    env->banked_r14[0] = regs.reg14[MODE_USER];
    env->regs[15] = regs.reg15;
    cpsr_write(env, regs.cpsr, 0xFFFFFFFF);
    regs.spsr[MODE_FIQ] = env->banked_spsr[5];
    regs.spsr[MODE_IRQ] = env->banked_spsr[4];
    regs.spsr[MODE_SUP] = env->banked_spsr[1];
    regs.spsr[MODE_ABORT] = env->banked_spsr[2];
    regs.spsr[MODE_UNDEF] = env->banked_spsr[3];

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

