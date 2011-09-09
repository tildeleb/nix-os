#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "amd64.h"

/*
 * To do:
 *	PteNX;
 *	mmukmapsync grot for >1 processor;
 *	replace vmap with newer version (no PDMAP);
 *	mmuinit machno != fix;
 *	mmuptcopy (PteSHARED trick?);
 *	calculate and map up to TMFM (conf crap);
 */

#define PPN(x)		((x)&~(PGSZ-1))

/*
 * Nemo: NB:
 * m->pml4 is always the same.
 * sched->procsave->flushtbl zeroes the entries in pml4 up to
 * pml4->daddr (which is the number of entries used by the user, or
 * the index in the upper page table for this entry)
 * the new process will fault and we populate again the page table
 * as needed.
 *
 * mmuptp[0] is used to keep a free list of pages.
 * mmuptp[1-3] are used to keep PT pages for each level
 * 4K pages: pml4 -> lvl3 -> lvl2 -> lvl1 ->pg
 * 2M pages: pml4 -> lvl3 -> lvl2 -> pg
 *
 * Therefore, we can't use pml4 in other processors. Each one
 * has to play the same trick at least, using its own pml4.
 * For NIX, we have to fill up the pml4 of the application core
 * so it wont fault.
 */

void
mmuflushtlb(u64int)
{

	m->tlbpurge++;
	if(m->pml4->daddr){
		memset(UINT2PTR(m->pml4->va), 0, m->pml4->daddr*sizeof(PTE));
		m->pml4->daddr = 0;
	}
	cr3put(m->pml4->pa);
}

void
mmuflush(void)
{
	Mpl pl;

	pl = splhi();
	up->newtlb = 1;
	mmuswitch(up);
	splx(pl);
}

static void
mmuptpfree(Proc* proc, int clear)
{
	int l;
	PTE *pte;
	Page **last, *page;

	for(l = 1; l < 4; l++){
		last = &proc->mmuptp[l];
		if(*last == nil)
			continue;
		for(page = *last; page != nil; page = page->next){
//what is right here? 2 or 1?
			if(l <= 2 && clear)
				memset(UINT2PTR(page->va), 0, PTSZ);
			pte = UINT2PTR(page->prev->va);
			pte[page->daddr] = 0;
			last = &page->next;
		}
		*last = proc->mmuptp[0];
		proc->mmuptp[0] = proc->mmuptp[l];
		proc->mmuptp[l] = nil;
	}

	m->pml4->daddr = 0;
}

static void
tabs(int n)
{
	int i;

	for(i = 0; i < n; i++)
		print("  ");
}

void
dumpptepg(int lvl, uintptr pa)
{
	PTE *pte;
	int tab, i;

	tab = 4 - lvl;
	pte = UINT2PTR(KADDR(pa));
	for(i = 0; i < PTSZ/sizeof(PTE); i++)
		if(pte[i] & PteP){
			tabs(tab);
			print("l%d %#p[%#05x]: %#ullx\n", lvl, pa, i, pte[i]);

			/* skip kernel mappings */
			if((pte[i]&PteU) == 0){
				tabs(tab+1);
				print("...kern...\n");
				continue;
			}
			if(lvl > 2)
				dumpptepg(lvl-1, PPN(pte[i]));
		}
}

void
dumpmmu(Proc *p)
{
	int i;
	Page *pg;

	print("proc %#p\n", p);
	for(i = 3; i > 0; i--){
		print("mmuptp[%d]:\n", i);
		for(pg = p->mmuptp[i]; pg != nil; pg = pg->next)
			print("\tva %#ullx ppn %#ullx d %#ulx\n",
				pg->va, pg->pa, pg->daddr);
	}
	print("pml4 %#ullx\n", m->pml4->pa);
	dumpptepg(4, m->pml4->pa);
}

/*
 * define this to disable the 4K page allocator.
 */
#define PTPALLOC

#ifdef PTPALLOC

static Page mmuptpfreelist;

static Page*
mmuptpalloc(void)
{
	void* va;
	Page *page;

	/*
	 * Do not really need a whole Page structure,
	 * but it makes testing this out a lot easier.
	 * Could keep a cache and free excess.
	 * Have to maintain any fiction for pexit?
	 */
	lock(&mmuptpfreelist);
	if((page = mmuptpfreelist.next) != nil){
		mmuptpfreelist.next = page->next;
		mmuptpfreelist.ref--;
		unlock(&mmuptpfreelist);

		if(page->ref++ != 0)
			panic("mmuptpalloc ref\n");
		page->prev = page->next = nil;
		memset(UINT2PTR(page->va), 0, PTSZ);

		return page;
	}
	unlock(&mmuptpfreelist);

	if((page = malloc(sizeof(Page))) == nil){
		print("mmuptpalloc Page\n");

		return nil;
	}
	if((va = mallocalign(PTSZ, PTSZ, 0, 0)) == nil){
		print("mmuptpalloc va\n");
		free(page);

		return nil;
	}

	page->va = PTR2UINT(va);
	page->pa = PADDR(va);
	page->ref = 1;

	return page;
}
#endif /* PTPALLOC */

void
mmuswitch(Proc* proc)
{
	PTE *pte;
	Page *page;

	if(proc->newtlb){
		/*
 		 * NIX: We cannot clear our page tables if they are going to
		 * be used in the AC
		 */
		if(proc->ac == nil)
			mmuptpfree(proc, 1);
		proc->newtlb = 0;
	}

	if(m->pml4->daddr){
if(proc->ac)print("mmuswitch: clear u for pml4\n");
		memset(UINT2PTR(m->pml4->va), 0, m->pml4->daddr*sizeof(PTE));
		m->pml4->daddr = 0;
	}

	pte = UINT2PTR(m->pml4->va);
	for(page = proc->mmuptp[3]; page != nil; page = page->next){
		pte[page->daddr] = PPN(page->pa)|PteU|PteRW|PteP;
		if(page->daddr >= m->pml4->daddr)
			m->pml4->daddr = page->daddr+1;
	}

	tssrsp0(STACKALIGN(PTR2UINT(proc->kstack+KSTACK)));
	cr3put(m->pml4->pa);
}

void
mmurelease(Proc* proc)
{
	Page *page, *next;

	mmuptpfree(proc, 0);

	for(page = proc->mmuptp[0]; page != nil; page = next){
		next = page->next;
		if(--page->ref)
			panic("mmurelease: page->ref %d\n", page->ref);
#ifdef PTPALLOC
		lock(&mmuptpfreelist);
		page->next = mmuptpfreelist.next;
		mmuptpfreelist.next = page;
		mmuptpfreelist.ref++;
		page->prev = nil;
		unlock(&mmuptpfreelist);
#else
		pagechainhead(page);
#endif /* PTPALLOC */
	}
	if(proc->mmuptp[0] && pga.r.p)
		wakeup(&pga.r);
	proc->mmuptp[0] = nil;

	tssrsp0(STACKALIGN(m->stack+MACHSTKSZ));
	cr3put(m->pml4->pa);
}

/*
 * pg->pgszi indicates the page size in m->pgsz[] used for the mapping.
 * For the user, it can be either 2*MiB or 1*GiB pages.
 * For 2*MiB pages, we use three levels, not four.
 * For 1*GiB pages, we use two levels.
 */
void
mmuput(uintptr va, uintmem pa, uint attr, Page *pg)
{
	int lvl, user, x, pgsz;
	PTE *pte;
	Page *page, *prev;
	Mpl pl;

	DBG("up %#p mmuput %#p %#Px %#ux\n", up, va, pa, attr);

	assert(pg->pgszi >= 0);
	pgsz = m->pgsz[pg->pgszi];
	if(pa & (pgsz-1))
		panic("mmuput: pa offset non zero: %#ullx\n", pa);
	if(attr & ~(PTEVALID|PTEWRITE|PTERONLY|PTEUSER|PTEUNCACHED))
		panic("mmuput: wrong attr bits: %#ux\n", attr);
	pa |= attr;
	pl = splhi();
	user = (va < KZERO);
	x = PTLX(va, 3);
	pte = UINT2PTR(m->pml4->va);
	pte += x;
	prev = m->pml4;
	for(lvl = 3; lvl >= 0; lvl--){
		if(user){
			if(pgsz == 2*MiB && lvl == 1)	 /* use 2M */
				break;
			if(pgsz == 1*GiB && lvl == 2)	/* use 1G */
				break;
		}
		for(page = up->mmuptp[lvl]; page != nil; page = page->next){
			if(page->prev == prev && page->daddr == x)
				break;
		}
		if(page == nil){
			if(up->mmuptp[0] == 0){
#ifdef PTPALLOC
				page = mmuptpalloc();
#else
				page = newpage(1, 0, 0, PTSZ);
				page->va = VA(kmap(page));
				assert(page->pa == PADDR(UINT2PTR(page->va)));
#endif /* PTPALLOC */
			}
			else {
				page = up->mmuptp[0];
				up->mmuptp[0] = page->next;
			}
			page->daddr = x;
			page->next = up->mmuptp[lvl];
			up->mmuptp[lvl] = page;
			page->prev = prev;
			*pte = PPN(page->pa)|PteU|PteRW|PteP;
			if(lvl == 3 && x >= m->pml4->daddr)
				m->pml4->daddr = x+1;
		}
		x = PTLX(va, lvl-1);
		pte = UINT2PTR(KADDR(PPN(*pte)));
		pte += x;
		prev = page;
	}

	*pte = pa|PteU;
	if(user)
		switch(pgsz){
		case 2*MiB:
		case 1*GiB:
			*pte |= PtePS;
			break;
		default:
			panic("mmuput: user pages must be 2M or 1G");
		}
	splx(pl);
	DBG("up %#p new pte %#p = %#llux\n", up, pte, *pte);

	invlpg(va);			/* only if old entry valid? */
}

static Lock mmukmaplock;
static Lock vmaplock;

#define PML4X(v)	PTLX((v), 3)
#define PDPX(v)		PTLX((v), 2)
#define PDX(v)		PTLX((v), 1)
#define PTX(v)		PTLX((v), 0)

int
mmukmapsync(uvlong va)
{
	USED(va);

	return 0;
}

static PTE
pdeget(uintptr va)
{
	PTE *pdp;

	if(va < 0xffffffffc0000000ull)
		panic("pdeget(%#p)", va);

	pdp = (PTE*)(PDMAP+PDX(PDMAP)*4096);

	return pdp[PDX(va)];
}

/*
 * Add kernel mappings for pa -> va for a section of size bytes.
 * Called only after the va range is known to be unoccupied.
 */
static int
pdmap(uintptr pa, int attr, uintptr va, usize size)
{
	uintptr pae;
	PTE *pd, *pde, *pt, *pte;
	int pdx, pgsz;

	pd = (PTE*)(PDMAP+PDX(PDMAP)*4096);

	for(pae = pa + size; pa < pae; pa += pgsz){
		pdx = PDX(va);
		pde = &pd[pdx];

		/*
		 * Check if it can be mapped using a big page,
		 * i.e. is big enough and starts on a suitable boundary.
		 * Assume processor can do it.
		 */
		if(ALIGNED(pa, PGLSZ(1)) && ALIGNED(va, PGLSZ(1)) && (pae-pa) >= PGLSZ(1)){
			assert(*pde == 0);
			*pde = pa|attr|PtePS|PteP;
			pgsz = PGLSZ(1);
		}
		else{
			if(*pde == 0){
/*
 * XXX: Shouldn't this be mmuptpalloc()?
 */
				/*
				 * Need a PTSZ physical allocator here.
				 * Because space will never be given back
				 * (see vunmap below), just malloc it so
				 * Ron can prove a point.
				*pde = pmalloc(PTSZ)|PteRW|PteP;
				 */
				void *alloc;

				alloc = mallocalign(PTSZ, PTSZ, 0, 0);
				if(alloc != nil){
					*pde = PADDR(alloc)|PteRW|PteP;
//print("*pde %#llux va %#p\n", *pde, va);
					memset((PTE*)(PDMAP+pdx*4096), 0, 4096);

				}
			}
			assert(*pde != 0);

			pt = (PTE*)(PDMAP+pdx*4096);
			pte = &pt[PTX(va)];
			assert(!(*pte & PteP));
			*pte = pa|attr|PteP;
			pgsz = PGLSZ(0);
		}
		va += pgsz;
	}

	return 0;
}

static int
findhole(PTE* a, int n, int count)
{
	int have, i;

	have = 0;
	for(i = 0; i < n; i++){
		if(a[i] == 0)
			have++;
		else
			have = 0;
		if(have >= count)
			return i+1 - have;
	}

	return -1;
}

/*
 * Look for free space in the vmap.
 */
static uintptr
vmapalloc(usize size)
{
	int i, n, o;
	PTE *pd, *pt;
	int pdsz, ptsz;

	pd = (PTE*)(PDMAP+PDX(PDMAP)*4096);
	pd += PDX(VMAP);
	pdsz = VMAPSZ/PGLSZ(1);

	/*
	 * Look directly in the PD entries if the size is
	 * larger than the range mapped by a single entry.
	 */
	if(size >= PGLSZ(1)){
		n = HOWMANY(size, PGLSZ(1));
		if((o = findhole(pd, pdsz, n)) != -1)
			return VMAP + o*PGLSZ(1);
		return 0;
	}

	/*
	 * Size is smaller than that mapped by a single PD entry.
	 * Look for an already mapped PT page that has room.
	 */
	n = HOWMANY(size, PGLSZ(0));
	ptsz = PGLSZ(0)/sizeof(PTE);
	for(i = 0; i < pdsz; i++){
		if(!(pd[i] & PteP) || (pd[i] & PtePS))
			continue;

		pt = (PTE*)(PDMAP+(PDX(VMAP)+i)*4096);
		if((o = findhole(pt, ptsz, n)) != -1)
			return VMAP + i*PGLSZ(1) + o*PGLSZ(0);
	}

	/*
	 * Nothing suitable, start using a new PD entry.
	 */
	if((o = findhole(pd, pdsz, 1)) != -1)
		return VMAP + o*PGLSZ(1);

	return 0;
}

void*
vmap(uintptr pa, usize size)
{
	uintptr va;
	usize o, sz;

	print("vmap(%#p, %lud) pc=%#p\n", pa, size, getcallerpc(&pa));

	if(m->machno != 0)
		panic("vmap");

	/*
	 * This is incomplete; the checks are not comprehensive
	 * enough.
	 * Sometimes the request is for an already-mapped piece
	 * of low memory, in which case just return a good value
	 * and hope that a corresponding vunmap of the address
	 * will have the same address.
	 * To do this properly will require keeping track of the
	 * mappings; perhaps something like kmap, but kmap probably
	 * can't be used early enough for some of the uses.
	 */
	if(pa+size < 1ull*MiB)
		return KADDR(pa);
	if(pa < 1ull*MiB)
		return nil;

	/*
	 * Might be asking for less than a page.
	 * This should have a smaller granularity if
	 * the page size is large.
	 */
	o = pa & ((1<<PGSHFT)-1);
	pa -= o;
	sz = ROUNDUP(size+o, PGSZ);

	if(pa == 0){
		print("vmap(0, %lud) pc=%#p\n", size, getcallerpc(&pa));
		return nil;
	}
	ilock(&vmaplock);
	if((va = vmapalloc(sz)) == 0 || pdmap(pa, PtePCD|PteRW, va, sz) < 0){
		iunlock(&vmaplock);
		return nil;
	}
	iunlock(&vmaplock);

	DBG("vmap(%#p, %lud) => %#p\n", pa+o, size, va+o);

	return UINT2PTR(va + o);
}

void
vunmap(void* v, usize size)
{
	uintptr va;

	DBG("vunmap(%#p, %lud)\n", v, size);

	if(m->machno != 0)
		panic("vunmap");

	/*
	 * See the comments above in vmap.
	 */
	va = PTR2UINT(v);
	if(va >= KZERO && va+size < KZERO+1ull*MiB)
		return;

	/*
	 * Here will have to deal with releasing any
	 * resources used for the allocation (e.g. page table
	 * pages).
	 */
	print("vunmap(%#p, %lud)\n", v, size);
}

int
mmuwalk(PTE* pml4, uintptr va, int level, PTE** ret, u64int (*alloc)(usize))
{
	int l;
	uintmem pa;
	PTE *pte;

	DBG("mmuwalk%d: va %#p level %d\n", m->machno, va, level);
	pte = &pml4[PTLX(va, 3)];
	for(l = 3; l >= 0; l--){
		if(l == level)
			break;
		if(!(*pte & PteP)){
			if(alloc == nil)
				break;
			pa = alloc(PTSZ);
			if(pa == ~0)
				return -1;
			memset(UINT2PTR(KADDR(pa)), 0, PTSZ);
			*pte = pa|PteRW|PteP;
		}
		else if(*pte & PtePS)
			break;
		pte = UINT2PTR(KADDR(PPN(*pte)));
		pte += PTLX(va, l-1);
	}
	*ret = pte;

	return l;
}

uintmem
mmuphysaddr(uintptr va)
{
	int l;
	PTE *pte;
	uintmem mask, pa;

	/*
	 * Given a VA, find the PA.
	 * This is probably not the right interface,
	 * but will do as an experiment. Usual
	 * question, should va be void* or uintptr?
	 */
	l = mmuwalk(UINT2PTR(m->pml4->va), va, 0, &pte, nil);
	DBG("physaddr: va %#p l %d\n", va, l);
	if(l < 0)
		return ~0;

	mask = PGLSZ(l)-1;
	pa = (*pte & ~mask) + (va & mask);

	DBG("physaddr: l %d va %#p pa %#llux\n", l, va, pa);

	return pa;
}

Page mach0pml4;

void
mmuinit(void)
{
	int l;
	uchar *p;
	PTE *pte, *pml4;
	Page *page;
	u64int o, pa, r, sz;

	archmmu();
	DBG("mach%d: %#p pml4 %#p npgsz %d\n", m->machno, m, m->pml4, m->npgsz);

	if(m->machno != 0){
		/* NIX: KLUDGE: Has to go when each mach is using
		 * its own page table
		 */
		p = UINT2PTR(m->stack);
		p += MACHSTKSZ;
		memmove(p, UINT2PTR(mach0pml4.va), PTSZ);
		m->pml4 = &m->pml4kludge;
		m->pml4->va = PTR2UINT(p);
		m->pml4->pa = PADDR(p);
		m->pml4->daddr = mach0pml4.daddr;	/* # of user mappings in pml4 */

		r = rdmsr(Efer);
		r |= Nxe;
		wrmsr(Efer, r);
		cr3put(m->pml4->pa);
		DBG("m %#p pml4 %#p\n", m, m->pml4);
		return;
	}

	page = &mach0pml4;
	page->pa = cr3get();
	page->va = PTR2UINT(KADDR(page->pa));

	m->pml4 = page;

	r = rdmsr(Efer);
	r |= Nxe;
	wrmsr(Efer, r);

	/*
	 * Set up the various kernel memory allocator limits:
	 * pmstart/pmend bound the unused physical memory;
	 * vmstart/vmend bound the total possible virtual memory
	 * used by the kernel;
	 * vmunused is the highest virtual address currently mapped
	 * and used by the kernel;
	 * vmunmapped is the highest virtual address currently
	 * mapped by the kernel.
	 * Vmunused can be bumped up to vmunmapped before more
	 * physical memory needs to be allocated and mapped.
	 *
	 * This is set up here so meminit can map appropriately.
	 */
#define TMFM		(64*MiB)		/* kernel memory */
	o = sys->pmstart;
	sz = ROUNDUP(o, 4*MiB) - o;
	pa = asmalloc(0, sz, 1, 0);
	if(pa != o)
		panic("mmuinit: pa %#llux memstart %#llux\n", pa, o);
	sys->pmstart += sz;

	sys->vmstart = KSEG0;
	sys->vmunused = sys->vmstart + ROUNDUP(o, 4*KiB);
	sys->vmunmapped = sys->vmstart + o + sz;
	sys->vmend = sys->vmstart + TMFM;

	print("mmuinit: vmstart %#p vmunused %#p vmunmapped %#p vmend %#p\n",
		sys->vmstart, sys->vmunused, sys->vmunmapped, sys->vmend);

	/*
	 * Set up the map for PD entry access by inserting
	 * the relevant PDP entry into the PD. It's equivalent
	 * to PADDR(sys->pd)|PteRW|PteP.
	 *
	 */
	sys->pd[PDX(PDMAP)] = sys->pdp[PDPX(PDMAP)] & ~(PteD|PteA);
	print("sys->pd %#p %#p\n", sys->pd[PDX(PDMAP)], sys->pdp[PDPX(PDMAP)]);
	assert((pdeget(PDMAP) & ~(PteD|PteA)) == (PADDR(sys->pd)|PteRW|PteP));


	pml4 = UINT2PTR(m->pml4->va);
	if((l = mmuwalk(pml4, KZERO, 3, &pte, nil)) >= 0)
		print("l %d %#p %llux\n", l, pte, *pte);
	if((l = mmuwalk(pml4, KZERO, 2, &pte, nil)) >= 0)
		print("l %d %#p %llux\n", l, pte, *pte);
	if((l = mmuwalk(pml4, KZERO, 1, &pte, nil)) >= 0)
		print("l %d %#p %llux\n", l, pte, *pte);
	if((l = mmuwalk(pml4, KZERO, 0, &pte, nil)) >= 0)
		print("l %d %#p %llux\n", l, pte, *pte);

	mmuphysaddr(PTR2UINT(end));
}