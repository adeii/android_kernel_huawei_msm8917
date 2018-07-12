/*
 * livepatch.c - arm64-specific Kernel Live Patching Core
 *
 * Copyright (C) 2014 Li Bin <huawei.libin@huawei.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/livepatch.h>
#include <asm/livepatch.h>
#include <asm/stacktrace.h>
#include <asm/cacheflush.h>
#include <linux/slab.h>
#include <asm/insn.h>
#include <asm-generic/sections.h>
#include <asm/ptrace.h>

static inline int in_entry_text(unsigned long ptr)
{
	return (ptr >= (unsigned long)__entry_text_start &&
		ptr < (unsigned long)__entry_text_end);
}
struct klp_func_node {
	struct list_head node;
	struct list_head func_stack;
	unsigned long old_addr;
	u32	old_insn;
};

static LIST_HEAD(klp_func_list);

/**
 * klp_write_module_reloc() - write a relocation in a module
 * @mod:	module in which the section to be modified is found
 * @type:	ELF relocation type (see asm/elf.h)
 * @loc:	address that the relocation should be written to
 * @value:	relocation value (sym address + addend)
 *
 * This function writes a relocation to the specified location for
 * a particular module.
 */
int klp_write_module_reloc(struct module *mod, unsigned long type,
			   unsigned long loc, unsigned long value)
{
	unsigned int readonly = 0;
	int ret = 0;
#ifdef CONFIG_DEBUG_SET_MODULE_RONX
	if (loc < (unsigned long)mod->module_core + mod->core_ro_size)
		readonly = 1;
#endif

	if (readonly)
		set_memory_rw(loc&PAGE_MASK, 1);

	/* Perform the static relocation. */
	ret = static_relocate(mod, type, (void *)loc, value);

	if (readonly)
		set_memory_ro(loc&PAGE_MASK, 1);
	return ret;
}

struct walk_stackframe_args {
	struct klp_patch *patch;
	int enable;
	int ret;
};

static inline int klp_compare_address(unsigned long pc, unsigned long func_addr,
				unsigned long func_size, const char *func_name)
{
	if (pc >= func_addr && pc < func_addr + func_size) {
		pr_err("func %s is in use!\n", func_name);
		return -EBUSY;
	}
	return 0;
}

static int klp_check_activeness_func(struct stackframe *frame, void *data)
{
	struct walk_stackframe_args *args = data;
	struct klp_patch *patch = args->patch;
	struct klp_object *obj;
	struct klp_func *func;
	unsigned long func_addr, func_size;
	const char *func_name;

	if (args->ret)
		return args->ret;

	for (obj = patch->objs; obj->funcs; obj++) {
		for (func = obj->funcs; func->old_name; func++) {
			if (args->enable) {
				if (func->force)
					continue;
				func_addr = func->old_addr;
				func_size = func->old_size;
			} else {
				func_addr = (unsigned long)func->new_func;
				func_size = func->new_size;
			}
			func_name = func->old_name;
			args->ret = klp_compare_address(frame->pc, func_addr, func_size, func_name);
			if (args->ret)
				return args->ret;
		}
	}

	return args->ret;
}

void notrace klp_walk_stackframe(struct stackframe *frame,
		int (*fn)(struct stackframe *, void *),
		struct task_struct *tsk, void *data)
{
	struct pt_regs *regs;
	unsigned long high, low;
	low = (unsigned long)task_stack_page(tsk);
	high = low + THREAD_SIZE;

	while (1) {
		int ret;

		if (fn(frame, data))
			break;
		ret = unwind_frame(frame);
		if (ret < 0)
			break;

		if (in_entry_text(frame->pc)) {
			regs = (struct pt_regs *)frame->sp;
			if (regs->sp < low || regs->sp > high -0x18)/*0x18 for sp2pc size */
				break;
			frame->pc = regs->pc;
			if (fn(frame, data))
				break;
			frame->sp = regs->sp;
			frame->fp = regs->regs[29];/*the 29th regs for fp pointer*/
			frame->pc = regs->regs[30];/*the 30th regs for pc pointer*/
		}
	}
}

int klp_check_calltrace(struct klp_patch *patch, int enable)
{
	struct task_struct *g, *t;
	struct stackframe frame;
	int ret = 0;

	struct walk_stackframe_args args = {
		.patch = patch,
		.enable = enable,
		.ret = 0
	};

	do_each_thread(g, t) {
		frame.fp = thread_saved_fp(t);
		frame.sp = thread_saved_sp(t);
		frame.pc = thread_saved_pc(t);
		klp_walk_stackframe(&frame, klp_check_activeness_func, t, &args);
		if (args.ret) {
			ret = args.ret;
			pr_info("PID: %d Comm: %.20s\n", t->pid, t->comm);
			show_stack(t, NULL);
			goto out;
		}
	} while_each_thread(g, t);

out:
	return ret;
}

static struct klp_func_node *klp_find_func_node(unsigned long old_addr)
{
	struct klp_func_node *func_node;

	list_for_each_entry(func_node, &klp_func_list, node) {
		if (func_node->old_addr == old_addr)
			return func_node;
	}

	return NULL;
}

int arch_klp_init_func(struct klp_object *obj, struct klp_func *func)
{
	return 0;
}

void arch_klp_free_func(struct klp_object *obj, struct klp_func *limit)
{
	return;
}

int arch_klp_enable_func(struct klp_func *func)
{
	struct klp_func_node *func_node;
	unsigned long pc, new_addr;
	u32 insn;
	u32 memory_flag = 0 ;


	func_node = klp_find_func_node(func->old_addr);
	if (!func_node) {
		func_node = kzalloc(sizeof(*func_node), GFP_ATOMIC);
		if (!func_node)
			return -ENOMEM;
		memory_flag = 1;

		INIT_LIST_HEAD(&func_node->func_stack);
		func_node->old_addr = func->old_addr;
		aarch64_insn_read((void *)func->old_addr, &func_node->old_insn);
		list_add_rcu(&func_node->node, &klp_func_list);
	}

	list_add_rcu(&func->stack_node, &func_node->func_stack);

	pc = func->old_addr;
	new_addr = (unsigned long)func->new_func;

	insn = aarch64_insn_gen_branch_imm(pc, new_addr,
			AARCH64_INSN_BRANCH_NOLINK);

	if (aarch64_insn_patch_text_nosync((void *)pc, insn))
		goto ERR_OUT;

	return 0;
ERR_OUT:
	if (memory_flag) {
		list_del_rcu(&func->stack_node);
		list_del_rcu(&func_node->node);
		kfree(func_node);
	}
    return -EPERM;
}

void arch_klp_disable_func(struct klp_func *func)
{
	struct klp_func_node *func_node;
	struct klp_func *next_func;
	unsigned long pc, new_addr;
	u32 insn;

	func_node = klp_find_func_node(func->old_addr);
	BUG_ON(!func_node);
	pc = func_node->old_addr;
	if (list_is_singular(&func_node->func_stack)) {
		insn = func_node->old_insn;
		list_del_rcu(&func->stack_node);
		list_del_rcu(&func_node->node);
		kfree(func_node);

		//insn = aarch64_insn_gen_nop();
		aarch64_insn_patch_text_nosync((void *)pc, insn);
	} else {
		list_del_rcu(&func->stack_node);
		next_func = list_first_or_null_rcu(&func_node->func_stack,
					struct klp_func, stack_node);
		BUG_ON(!next_func);
		new_addr = (unsigned long)next_func->new_func;
		insn = aarch64_insn_gen_branch_imm(pc, new_addr,
				AARCH64_INSN_BRANCH_NOLINK);

		aarch64_insn_patch_text_nosync((void *)pc, insn);
	}
}
