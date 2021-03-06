#define DISABLE_BRANCH_PROFILING
#define pr_fmt(fmt) "kasan: " fmt
#include <linux/bootmem.h>
#include <linux/kasan.h>
#include <linux/kdebug.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/vmalloc.h>

#include <asm/e820/types.h>
#include <asm/tlbflush.h>
#include <asm/sections.h>

extern pgd_t early_top_pgt[PTRS_PER_PGD];
extern struct range pfn_mapped[E820_MAX_ENTRIES];

static int __init map_range(struct range *range)
{
	unsigned long start;
	unsigned long end;

	start = (unsigned long)kasan_mem_to_shadow(pfn_to_kaddr(range->start));
	end = (unsigned long)kasan_mem_to_shadow(pfn_to_kaddr(range->end));

	return vmemmap_populate(start, end, NUMA_NO_NODE);
}

static void __init clear_pgds(unsigned long start,
			unsigned long end)
{
	pgd_t *pgd;

	for (; start < end; start += PGDIR_SIZE) {
		pgd = pgd_offset_k(start);
		/*
		 * With folded p4d, pgd_clear() is nop, use p4d_clear()
		 * instead.
		 */
		if (CONFIG_PGTABLE_LEVELS < 5)
			p4d_clear(p4d_offset(pgd, start));
		else
			pgd_clear(pgd);
	}
}

static void __init kasan_map_early_shadow(pgd_t *pgd)
{
	int i;
	unsigned long start = KASAN_SHADOW_START;
	unsigned long end = KASAN_SHADOW_END;

	for (i = pgd_index(start); start < end; i++) {
		switch (CONFIG_PGTABLE_LEVELS) {
		case 4:
			pgd[i] = __pgd(__pa_nodebug(kasan_zero_pud) |
					_KERNPG_TABLE);
			break;
		case 5:
			pgd[i] = __pgd(__pa_nodebug(kasan_zero_p4d) |
					_KERNPG_TABLE);
			break;
		default:
			BUILD_BUG();
		}
		start += PGDIR_SIZE;
	}
}

#ifdef CONFIG_KASAN_INLINE
static int kasan_die_handler(struct notifier_block *self,
			     unsigned long val,
			     void *data)
{
	if (val == DIE_GPF) {
		pr_emerg("CONFIG_KASAN_INLINE enabled\n");
		pr_emerg("GPF could be caused by NULL-ptr deref or user memory access\n");
	}
	return NOTIFY_OK;
}

static struct notifier_block kasan_die_notifier = {
	.notifier_call = kasan_die_handler,
};
#endif

void __init kasan_early_init(void)
{
	int i;
	pteval_t pte_val = __pa_nodebug(kasan_zero_page) | __PAGE_KERNEL;
	pmdval_t pmd_val = __pa_nodebug(kasan_zero_pte) | _KERNPG_TABLE;
	pudval_t pud_val = __pa_nodebug(kasan_zero_pmd) | _KERNPG_TABLE;
	p4dval_t p4d_val = __pa_nodebug(kasan_zero_pud) | _KERNPG_TABLE;

	for (i = 0; i < PTRS_PER_PTE; i++)
		kasan_zero_pte[i] = __pte(pte_val);

	for (i = 0; i < PTRS_PER_PMD; i++)
		kasan_zero_pmd[i] = __pmd(pmd_val);

	for (i = 0; i < PTRS_PER_PUD; i++)
		kasan_zero_pud[i] = __pud(pud_val);

	for (i = 0; CONFIG_PGTABLE_LEVELS >= 5 && i < PTRS_PER_P4D; i++)
		kasan_zero_p4d[i] = __p4d(p4d_val);

	kasan_map_early_shadow(early_top_pgt);
	kasan_map_early_shadow(init_top_pgt);
}

void __init kasan_init(void)
{
	int i;

#ifdef CONFIG_KASAN_INLINE
	register_die_notifier(&kasan_die_notifier);
#endif

	memcpy(early_top_pgt, init_top_pgt, sizeof(early_top_pgt));
	load_cr3(early_top_pgt);
	__flush_tlb_all();

	clear_pgds(KASAN_SHADOW_START, KASAN_SHADOW_END);

	kasan_populate_zero_shadow((void *)KASAN_SHADOW_START,
			kasan_mem_to_shadow((void *)PAGE_OFFSET));

	for (i = 0; i < E820_MAX_ENTRIES; i++) {
		if (pfn_mapped[i].end == 0)
			break;

		if (map_range(&pfn_mapped[i]))
			panic("kasan: unable to allocate shadow!");
	}
	kasan_populate_zero_shadow(
		kasan_mem_to_shadow((void *)PAGE_OFFSET + MAXMEM),
		kasan_mem_to_shadow((void *)__START_KERNEL_map));

	vmemmap_populate((unsigned long)kasan_mem_to_shadow(_stext),
			(unsigned long)kasan_mem_to_shadow(_end),
			NUMA_NO_NODE);

	kasan_populate_zero_shadow(kasan_mem_to_shadow((void *)MODULES_END),
			(void *)KASAN_SHADOW_END);

	load_cr3(init_top_pgt);
	__flush_tlb_all();

	/*
	 * kasan_zero_page has been used as early shadow memory, thus it may
	 * contain some garbage. Now we can clear and write protect it, since
	 * after the TLB flush no one should write to it.
	 */
	memset(kasan_zero_page, 0, PAGE_SIZE);
	for (i = 0; i < PTRS_PER_PTE; i++) {
		pte_t pte = __pte(__pa(kasan_zero_page) | __PAGE_KERNEL_RO);
		set_pte(&kasan_zero_pte[i], pte);
	}
	/* Flush TLBs again to be sure that write protection applied. */
	__flush_tlb_all();

	init_task.kasan_depth = 0;
	pr_info("KernelAddressSanitizer initialized\n");
}
