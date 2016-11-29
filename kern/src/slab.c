/* Copyright (c) 2009 The Regents of the University of California
 * See LICENSE for details.
 *
 * Slab allocator, based on the SunOS 5.4 allocator paper.
 *
 * Barret Rhoden <brho@cs.berkeley.edu>
 * Kevin Klues <klueska@cs.berkeley.edu>
 *
 * Copyright (c) 2016 Google Inc
 *
 * Upgraded and extended to support magazines, based on Bonwick and Adams's
 * "Magazines and Vmem" paper.
 *
 * Barret Rhoden <brho@cs.berkeley.edu>
 *
 * FAQ:
 * - What sort of allocator do we need for the kmem_pcpu_caches?  In general,
 *   the base allocator.  All slabs/caches depend on the pcpu_caches for any
 *   allocation, so we need something that does not rely on slabs.  We could use
 *   generic kpages, if we knew that we weren't: qcaches for a kpages_arena, the
 *   slab kcache, or the bufctl kcache.  This is the same set of restrictions
 *   for the hash table allocations.
 * - Why doesn't the magazine cache deadlock on itself?  Because magazines are
 *   only allocated during the free path of another cache.  There are no
 *   magazine allocations during a cache's allocation.
 * - Does the magazine cache need to be statically allocated?  Maybe not, but it
 *   doesn't hurt.  We need to set it up at some point.  We can use other caches
 *   for allocations before the mag cache is initialized, but we can't free.
 * - Does the magazine cache need to pull from the base arena?  Similar to the
 *   static allocation question - by default, maybe not, but it is safer.  And
 *   yes, due to other design choices.  We could initialize it after kpages is
 *   allocated and use a kpages_arena, but that would require us to not free a
 *   page before or during kpages_arena_init().  A related note is where the
 *   first magazines in a pcpu_cache come from.  I'm currently going with "raw
 *   slab alloc from the magazine cache", which means magazines need to work
 *   when we're setting up the qcache's for kpages_arena.  That creates a
 *   dependency, which means kpages depends on mags, which means mags can only
 *   depend on base.  If we ever use slabs for non-base arena btags, we'll also
 *   have this dependency between kpages and mags.
 * - The paper talks about full and empty magazines.  Why does our code talk
 *   about not_empty and empty?  The way we'll do our magazine resizing is to
 *   just() increment the pcpu_cache's magsize.  Then we'll eventually start
 *   filling the magazines to their new capacity (during frees, btw).  During
 *   this time, a mag that was previously full will technically be not-empty,
 *   but not full.  The correctness of the magazine code is still OK, I think,
 *   since when they say 'full', they require 'not empty' in most cases.  In
 *   short, 'not empty' is more accurate, though it makes sense to say 'full'
 *   when explaining the basic idea for their paper.
 * - Due to a resize, what happens when the depot gives a pcpu cache a magazine
 *   with *more* rounds than ppc->magsize?  The allocation path doesn't care
 *   about magsize - it just looks at nr_rounds.  So that's fine.  On the free
 *   path, we might mistakenly think that a mag has no more room.  In that case,
 *   we'll just hand it to the depot and it'll be a 'not-empty' mag.  Eventually
 *   it'll get filled up, or it just won't matter.  'magsize' is basically an
 *   instruction to the pcpu_cache: "fill to X, please."
 * - Why is nr_rounds tracked in the magazine and not the pcpu cache?  The paper
 *   uses the pcpu cache, but doesn't say whether or not the mag tracks it too.
 *   We track it in the mag since not all mags have the same size (e.g.  during
 *   a resize operation).  For performance (avoid an occasional cache miss), we
 *   could consider tracking it in the pcpu_cache.  Might save a miss now and
 *   then.
 * - Why do we just disable IRQs for the pcpu_cache?  The paper explicitly talks
 *   about using locks instead of disabling IRQs, since disabling IRQs can be
 *   expensive.  First off, we only just disable IRQs when there's 1:1 core to
 *   pcc.  If we were to use a spinlock, we'd be disabling IRQs anyway, since we
 *   do allocations from IRQ context.  The other reason to lock is when changing
 *   the pcpu state during a magazine resize.  I have two ways to do this: just
 *   racily write and set pcc->magsize, or have the pcc's poll when they check
 *   the depot during free.  Either approach doesn't require someone else to
 *   grab a pcc lock.
 *
 * TODO:
 * - Add reclaim function.
 * - When resizing, do we want to go through the depot and consolidate
 *   magazines?  (probably not a big deal.  maybe we'd deal with it when we
 *   clean up our excess mags.)
 * - Could do some working set tracking.  Like max/min over an interval, with
 *   resetting (in the depot, used for reclaim and maybe aggressive freeing).
 * - Debugging info
 */

#include <slab.h>
#include <stdio.h>
#include <assert.h>
#include <pmap.h>
#include <kmalloc.h>
#include <hash.h>
#include <arena.h>

#define SLAB_POISON ((void*)0xdead1111)

/* Tunables.  I don't know which numbers to pick yet.  Maybe we play with it at
 * runtime.  Though once a mag increases, it'll never decrease. */
uint64_t resize_timeout_ns = 1000000000;
unsigned int resize_threshold = 1;

/* Protected by the arenas_and_slabs_lock. */
struct kmem_cache_tailq all_kmem_caches =
		TAILQ_HEAD_INITIALIZER(all_kmem_caches);

/* Backend/internal functions, defined later.  Grab the lock before calling
 * these. */
static bool kmem_cache_grow(struct kmem_cache *cp);
static void *__kmem_alloc_from_slab(struct kmem_cache *cp, int flags);
static void __kmem_free_to_slab(struct kmem_cache *cp, void *buf);

/* Cache of the kmem_cache objects, needed for bootstrapping */
struct kmem_cache kmem_cache_cache[1];
struct kmem_cache kmem_slab_cache[1];
struct kmem_cache kmem_bufctl_cache[1];
struct kmem_cache kmem_magazine_cache[1];

static bool __use_bufctls(struct kmem_cache *cp)
{
	return cp->flags & __KMC_USE_BUFCTL;
}

/* Using a layer of indirection for the pcpu caches, in case we want to use
 * clustered objects, only per-NUMA-domain caches, or something like that. */
unsigned int kmc_nr_pcpu_caches(void)
{
	return num_cores;
}

static struct kmem_pcpu_cache *get_my_pcpu_cache(struct kmem_cache *kc)
{
	return &kc->pcpu_caches[core_id()];
}

/* In our current model, there is one pcc per core.  If we had multiple cores
 * that could use the pcc, such as with per-NUMA caches, then we'd need a
 * spinlock.  Since we do allocations from IRQ context, we still need to disable
 * IRQs. */
static void lock_pcu_cache(struct kmem_pcpu_cache *pcc)
{
	disable_irqsave(&pcc->irq_state);
}

static void unlock_pcu_cache(struct kmem_pcpu_cache *pcc)
{
	enable_irqsave(&pcc->irq_state);
}

static void lock_depot(struct kmem_depot *depot)
{
	uint64_t time;

	if (spin_trylock_irqsave(&depot->lock))
		return;
	/* The lock is contended.  When we finally get the lock, we'll up the
	 * contention count and see if we've had too many contentions over time.
	 *
	 * The idea is that if there are bursts of contention worse than X contended
	 * acquisitions in Y nsec, then we'll grow the magazines.  This might not be
	 * that great of an approach - every thread gets one count, regardless of
	 * how long they take.
	 *
	 * We read the time before locking so that we don't artificially grow the
	 * window too much.  Say the lock is heavily contended and we take a long
	 * time to get it.  Perhaps X threads try to lock it immediately, but it
	 * takes over Y seconds for the Xth thread to actually get the lock.  We
	 * might then think the burst wasn't big enough. */
	time = nsec();
	spin_lock_irqsave(&depot->lock);
	/* If there are no not-empty mags, we're probably fighting for the lock not
	 * because the magazines aren't big enough, but because there aren't enough
	 * mags in the system yet. */
	if (!depot->nr_not_empty)
		return;
	if (time - depot->busy_start > resize_timeout_ns) {
		depot->busy_count = 0;
		depot->busy_start = time;
	}
	depot->busy_count++;
	if (depot->busy_count > resize_threshold) {
		depot->busy_count = 0;
		depot->magsize = MIN(KMC_MAG_MAX_SZ, depot->magsize + 1);
		/* That's all we do - the pccs will eventually notice and up their
		 * magazine sizes. */
	}
}

static void unlock_depot(struct kmem_depot *depot)
{
	spin_unlock_irqsave(&depot->lock);
}

static void depot_init(struct kmem_depot *depot)
{
	spinlock_init_irqsave(&depot->lock);
	SLIST_INIT(&depot->not_empty);
	SLIST_INIT(&depot->empty);
	depot->magsize = KMC_MAG_MIN_SZ;
	depot->nr_not_empty = 0;
	depot->nr_empty = 0;
	depot->busy_count = 0;
	depot->busy_start = 0;
}

static bool mag_is_empty(struct kmem_magazine *mag)
{
	return mag->nr_rounds == 0;
}

/* Helper, swaps the loaded and previous mags.  Hold the pcc lock. */
static void __swap_mags(struct kmem_pcpu_cache *pcc)
{
	struct kmem_magazine *temp;

	temp = pcc->prev;
	pcc->prev = pcc->loaded;
	pcc->loaded = temp;
}

/* Helper, returns a magazine to the depot.  Hold the depot lock. */
static void __return_to_depot(struct kmem_cache *kc, struct kmem_magazine *mag)
{
	struct kmem_depot *depot = &kc->depot;

	if (mag_is_empty(mag)) {
		SLIST_INSERT_HEAD(&depot->empty, mag, link);
		depot->nr_empty++;
	} else {
		SLIST_INSERT_HEAD(&depot->not_empty, mag, link);
		depot->nr_not_empty++;
	}
}

/* Helper, removes the contents of the magazine, giving them back to the slab
 * layer. */
static void drain_mag(struct kmem_cache *kc, struct kmem_magazine *mag)
{
	for (int i = 0; i < mag->nr_rounds; i++) {
		if (kc->dtor)
			kc->dtor(mag->rounds[i], kc->priv);
		__kmem_free_to_slab(kc, mag->rounds[i]);
	}
	mag->nr_rounds = 0;
}

static struct kmem_pcpu_cache *build_pcpu_caches(void)
{
	struct kmem_pcpu_cache *pcc;

	pcc = base_alloc(NULL,
	                 sizeof(struct kmem_pcpu_cache) * kmc_nr_pcpu_caches(),
	                 MEM_WAIT);
	for (int i = 0; i < kmc_nr_pcpu_caches(); i++) {
		pcc[i].irq_state = 0;
		pcc[i].magsize = KMC_MAG_MIN_SZ;
		pcc[i].loaded = __kmem_alloc_from_slab(kmem_magazine_cache, MEM_WAIT);
		pcc[i].prev = __kmem_alloc_from_slab(kmem_magazine_cache, MEM_WAIT);
		pcc[i].nr_allocs_ever = 0;
	}
	return pcc;
}

void __kmem_cache_create(struct kmem_cache *kc, const char *name,
                         size_t obj_size, int align, int flags,
                         struct arena *source,
                         int (*ctor)(void *, void *, int),
                         void (*dtor)(void *, void *), void *priv)
{
	assert(kc);
	assert(align);
	spinlock_init_irqsave(&kc->cache_lock);
	strlcpy(kc->name, name, KMC_NAME_SZ);
	kc->obj_size = ROUNDUP(obj_size, align);
	if (flags & KMC_QCACHE)
		kc->import_amt = ROUNDUPPWR2(3 * source->qcache_max);
	else
		kc->import_amt = ROUNDUP(NUM_BUF_PER_SLAB * obj_size, PGSIZE);
	kc->align = align;
	if (align > PGSIZE)
		panic("Cache %s object alignment is actually MIN(PGSIZE, align (%p))",
		      name, align);
	kc->flags = flags;
	/* We might want some sort of per-call site NUMA-awareness in the future. */
	kc->source = source ? source : kpages_arena;
	TAILQ_INIT(&kc->full_slab_list);
	TAILQ_INIT(&kc->partial_slab_list);
	TAILQ_INIT(&kc->empty_slab_list);
	kc->ctor = ctor;
	kc->dtor = dtor;
	kc->priv = priv;
	kc->nr_cur_alloc = 0;
	kc->alloc_hash = kc->static_hash;
	hash_init_hh(&kc->hh);
	for (int i = 0; i < kc->hh.nr_hash_lists; i++)
		BSD_LIST_INIT(&kc->static_hash[i]);
	/* No touch must use bufctls, even for small objects, so that it does not
	 * use the object as memory.  Note that if we have an arbitrary source,
	 * small objects, and we're 'pro-touch', the small allocation path will
	 * assume we're importing from a PGSIZE-aligned source arena. */
	if ((obj_size > SLAB_LARGE_CUTOFF) || (flags & KMC_NOTOUCH))
		kc->flags |= __KMC_USE_BUFCTL;
	depot_init(&kc->depot);
	/* We do this last, since this will all into the magazine cache - which we
	 * could be creating on this call! */
	kc->pcpu_caches = build_pcpu_caches();
	add_importing_slab(kc->source, kc);
	qlock(&arenas_and_slabs_lock);
	TAILQ_INSERT_TAIL(&all_kmem_caches, kc, all_kmc_link);
	qunlock(&arenas_and_slabs_lock);
}

static int __mag_ctor(void *obj, void *priv, int flags)
{
	struct kmem_magazine *mag = (struct kmem_magazine*)obj;

	mag->nr_rounds = 0;
	return 0;
}

void kmem_cache_init(void)
{
	/* magazine must be first - all caches, including mags, will do a slab alloc
	 * from the mag cache. */
	static_assert(sizeof(struct kmem_magazine) <= SLAB_LARGE_CUTOFF);
	__kmem_cache_create(kmem_magazine_cache, "kmem_magazine",
	                    sizeof(struct kmem_magazine),
	                    __alignof__(struct kmem_magazine), 0, base_arena,
	                    __mag_ctor, NULL, NULL);
	__kmem_cache_create(kmem_cache_cache, "kmem_cache",
	                    sizeof(struct kmem_cache),
	                    __alignof__(struct kmem_cache), 0, base_arena,
	                    NULL, NULL, NULL);
	__kmem_cache_create(kmem_slab_cache, "kmem_slab",
	                    sizeof(struct kmem_slab),
	                    __alignof__(struct kmem_slab), 0, base_arena,
	                    NULL, NULL, NULL);
	__kmem_cache_create(kmem_bufctl_cache, "kmem_bufctl",
	                    sizeof(struct kmem_bufctl),
	                    __alignof__(struct kmem_bufctl), 0, base_arena,
	                    NULL, NULL, NULL);
}

/* Cache management */
struct kmem_cache *kmem_cache_create(const char *name, size_t obj_size,
                                     int align, int flags,
                                     struct arena *source,
                                     int (*ctor)(void *, void *, int),
                                     void (*dtor)(void *, void *),
                                     void *priv)
{
	struct kmem_cache *kc = kmem_cache_alloc(kmem_cache_cache, 0);

	__kmem_cache_create(kc, name, obj_size, align, flags, source, ctor, dtor,
	                    priv);
	return kc;
}

/* Helper during destruction.  No one should be touching the allocator anymore.
 * We just need to hand objects back to the depot, which will hand them to the
 * slab.  Locking is just a formality here. */
static void drain_pcpu_caches(struct kmem_cache *kc)
{
	struct kmem_pcpu_cache *pcc;

	for (int i = 0; i < kmc_nr_pcpu_caches(); i++) {
		pcc = &kc->pcpu_caches[i];
		lock_pcu_cache(pcc);
		lock_depot(&kc->depot);
		__return_to_depot(kc, pcc->loaded);
		__return_to_depot(kc, pcc->prev);
		unlock_depot(&kc->depot);
		pcc->loaded = SLAB_POISON;
		pcc->prev = SLAB_POISON;
		unlock_pcu_cache(pcc);
	}
}

static void depot_destroy(struct kmem_cache *kc)
{
	struct kmem_magazine *mag_i;
	struct kmem_depot *depot = &kc->depot;

	lock_depot(depot);
	while ((mag_i = SLIST_FIRST(&depot->not_empty))) {
		drain_mag(kc, mag_i);
		kmem_cache_free(kmem_magazine_cache, mag_i);
	}
	while ((mag_i = SLIST_FIRST(&depot->empty)))
		kmem_cache_free(kmem_magazine_cache, mag_i);
	unlock_depot(depot);
}

static void kmem_slab_destroy(struct kmem_cache *cp, struct kmem_slab *a_slab)
{
	if (!__use_bufctls(cp)) {
		arena_free(cp->source, ROUNDDOWN(a_slab, PGSIZE), PGSIZE);
	} else {
		struct kmem_bufctl *i, *temp;
		void *buf_start = (void*)SIZE_MAX;

		BSD_LIST_FOREACH_SAFE(i, &a_slab->bufctl_freelist, link, temp) {
			// Track the lowest buffer address, which is the start of the buffer
			buf_start = MIN(buf_start, i->buf_addr);
			/* This is a little dangerous, but we can skip removing, since we
			 * init the freelist when we reuse the slab. */
			kmem_cache_free(kmem_bufctl_cache, i);
		}
		arena_free(cp->source, buf_start, cp->import_amt);
		kmem_cache_free(kmem_slab_cache, a_slab);
	}
}

/* Once you call destroy, never use this cache again... o/w there may be weird
 * races, and other serious issues.  */
void kmem_cache_destroy(struct kmem_cache *cp)
{
	struct kmem_slab *a_slab, *next;

	qlock(&arenas_and_slabs_lock);
	TAILQ_REMOVE(&all_kmem_caches, cp, all_kmc_link);
	qunlock(&arenas_and_slabs_lock);
	del_importing_slab(cp->source, cp);
	drain_pcpu_caches(cp);
	depot_destroy(cp);
	spin_lock_irqsave(&cp->cache_lock);
	assert(TAILQ_EMPTY(&cp->full_slab_list));
	assert(TAILQ_EMPTY(&cp->partial_slab_list));
	/* Clean out the empty list.  We can't use a regular FOREACH here, since the
	 * link element is stored in the slab struct, which is stored on the page
	 * that we are freeing. */
	a_slab = TAILQ_FIRST(&cp->empty_slab_list);
	while (a_slab) {
		next = TAILQ_NEXT(a_slab, link);
		kmem_slab_destroy(cp, a_slab);
		a_slab = next;
	}
	spin_unlock_irqsave(&cp->cache_lock);
	kmem_cache_free(kmem_cache_cache, cp);
}

static void __try_hash_resize(struct kmem_cache *cp)
{
	struct kmem_bufctl_list *new_tbl, *old_tbl;
	struct kmem_bufctl *bc_i;
	unsigned int new_tbl_nr_lists, old_tbl_nr_lists;
	size_t new_tbl_sz, old_tbl_sz;
	size_t hash_idx;

	if (!hash_needs_more(&cp->hh))
		return;
	new_tbl_nr_lists = hash_next_nr_lists(&cp->hh);
	new_tbl_sz = new_tbl_nr_lists * sizeof(struct kmem_bufctl_list);
	/* TODO: we only need to pull from base if our arena is a base or we are
	 * inside a kpages arena (keep in mind there could be more than one of
	 * those, depending on how we do NUMA allocs).  This might help with
	 * fragmentation.  To know this, we'll need the caller to pass us a flag. */
	new_tbl = base_zalloc(NULL, new_tbl_sz, ARENA_INSTANTFIT | MEM_ATOMIC);
	if (!new_tbl)
		return;
	old_tbl = cp->alloc_hash;
	old_tbl_nr_lists = cp->hh.nr_hash_lists;
	old_tbl_sz = old_tbl_nr_lists * sizeof(struct kmem_bufctl_list);
	cp->alloc_hash = new_tbl;
	hash_incr_nr_lists(&cp->hh);
	for (int i = 0; i < old_tbl_nr_lists; i++) {
		while ((bc_i = BSD_LIST_FIRST(&old_tbl[i]))) {
			BSD_LIST_REMOVE(bc_i, link);
			hash_idx = hash_ptr(bc_i->buf_addr, cp->hh.nr_hash_bits);
			BSD_LIST_INSERT_HEAD(&cp->alloc_hash[hash_idx], bc_i, link);
		}
	}
	hash_reset_load_limit(&cp->hh);
	if (old_tbl != cp->static_hash)
		base_free(NULL, old_tbl, old_tbl_sz);
}

/* Helper, tracks the allocation of @bc in the hash table */
static void __track_alloc(struct kmem_cache *cp, struct kmem_bufctl *bc)
{
	size_t hash_idx;

	hash_idx = hash_ptr(bc->buf_addr, cp->hh.nr_hash_bits);
	BSD_LIST_INSERT_HEAD(&cp->alloc_hash[hash_idx], bc, link);
	cp->hh.nr_items++;
	__try_hash_resize(cp);
}

/* Helper, looks up and removes the bufctl corresponding to buf. */
static struct kmem_bufctl *__yank_bufctl(struct kmem_cache *cp, void *buf)
{
	struct kmem_bufctl *bc_i;
	size_t hash_idx;

	hash_idx = hash_ptr(buf, cp->hh.nr_hash_bits);
	BSD_LIST_FOREACH(bc_i, &cp->alloc_hash[hash_idx], link) {
		if (bc_i->buf_addr == buf) {
			BSD_LIST_REMOVE(bc_i, link);
			break;
		}
	}
	if (!bc_i)
		panic("Could not find buf %p in cache %s!", buf, cp->name);
	return bc_i;
}

/* Alloc, bypassing the magazines and depot */
static void *__kmem_alloc_from_slab(struct kmem_cache *cp, int flags)
{
	void *retval = NULL;
	spin_lock_irqsave(&cp->cache_lock);
	// look at partial list
	struct kmem_slab *a_slab = TAILQ_FIRST(&cp->partial_slab_list);
	// 	if none, go to empty list and get an empty and make it partial
	if (!a_slab) {
		// TODO: think about non-sleeping flags
		if (TAILQ_EMPTY(&cp->empty_slab_list) &&
			!kmem_cache_grow(cp)) {
			spin_unlock_irqsave(&cp->cache_lock);
			if (flags & MEM_ERROR)
				error(ENOMEM, ERROR_FIXME);
			else
				panic("[German Accent]: OOM for a small slab growth!!!");
		}
		// move to partial list
		a_slab = TAILQ_FIRST(&cp->empty_slab_list);
		TAILQ_REMOVE(&cp->empty_slab_list, a_slab, link);
		TAILQ_INSERT_HEAD(&cp->partial_slab_list, a_slab, link);
	}
	// have a partial now (a_slab), get an item, return item
	if (!__use_bufctls(cp)) {
		retval = a_slab->free_small_obj;
		/* the next free_small_obj address is stored at the beginning of the
		 * current free_small_obj. */
		a_slab->free_small_obj = *(uintptr_t**)(a_slab->free_small_obj);
	} else {
		// rip the first bufctl out of the partial slab's buf list
		struct kmem_bufctl *a_bufctl = BSD_LIST_FIRST(&a_slab->bufctl_freelist);

		BSD_LIST_REMOVE(a_bufctl, link);
		__track_alloc(cp, a_bufctl);
		retval = a_bufctl->buf_addr;
	}
	a_slab->num_busy_obj++;
	// Check if we are full, if so, move to the full list
	if (a_slab->num_busy_obj == a_slab->num_total_obj) {
		TAILQ_REMOVE(&cp->partial_slab_list, a_slab, link);
		TAILQ_INSERT_HEAD(&cp->full_slab_list, a_slab, link);
	}
	cp->nr_cur_alloc++;
	spin_unlock_irqsave(&cp->cache_lock);
	if (cp->ctor) {
		if (cp->ctor(retval, cp->priv, flags)) {
			warn("Ctor %p failed, probably a bug!");
			__kmem_free_to_slab(cp, retval);
			return NULL;
		}
	}
	return retval;
}

void *kmem_cache_alloc(struct kmem_cache *kc, int flags)
{
	struct kmem_pcpu_cache *pcc = get_my_pcpu_cache(kc);
	struct kmem_depot *depot = &kc->depot;
	struct kmem_magazine *mag;
	void *ret;

	lock_pcu_cache(pcc);
try_alloc:
	if (pcc->loaded->nr_rounds) {
		ret = pcc->loaded->rounds[pcc->loaded->nr_rounds - 1];
		pcc->loaded->nr_rounds--;
		pcc->nr_allocs_ever++;
		unlock_pcu_cache(pcc);
		return ret;
	}
	if (!mag_is_empty(pcc->prev)) {
		__swap_mags(pcc);
		goto try_alloc;
	}
	/* Note the lock ordering: pcc -> depot */
	lock_depot(depot);
	mag = SLIST_FIRST(&depot->not_empty);
	if (mag) {
		SLIST_REMOVE_HEAD(&depot->not_empty, link);
		depot->nr_not_empty--;
		__return_to_depot(kc, pcc->prev);
		unlock_depot(depot);
		pcc->prev = pcc->loaded;
		pcc->loaded = mag;
		goto try_alloc;
	}
	unlock_depot(depot);
	unlock_pcu_cache(pcc);
	return __kmem_alloc_from_slab(kc, flags);
}

/* Returns an object to the slab layer.  Caller must deconstruct the objects.
 * Note that objects in the slabs are unconstructed. */
static void __kmem_free_to_slab(struct kmem_cache *cp, void *buf)
{
	struct kmem_slab *a_slab;
	struct kmem_bufctl *a_bufctl;

	spin_lock_irqsave(&cp->cache_lock);
	if (!__use_bufctls(cp)) {
		// find its slab
		a_slab = (struct kmem_slab*)(ROUNDDOWN((uintptr_t)buf, PGSIZE) +
		                             PGSIZE - sizeof(struct kmem_slab));
		/* write location of next free small obj to the space at the beginning
		 * of the buffer, then list buf as the next free small obj */
		*(uintptr_t**)buf = a_slab->free_small_obj;
		a_slab->free_small_obj = buf;
	} else {
		/* Give the bufctl back to the parent slab */
		a_bufctl = __yank_bufctl(cp, buf);
		a_slab = a_bufctl->my_slab;
		BSD_LIST_INSERT_HEAD(&a_slab->bufctl_freelist, a_bufctl, link);
	}
	a_slab->num_busy_obj--;
	cp->nr_cur_alloc--;
	// if it was full, move it to partial
	if (a_slab->num_busy_obj + 1 == a_slab->num_total_obj) {
		TAILQ_REMOVE(&cp->full_slab_list, a_slab, link);
		TAILQ_INSERT_HEAD(&cp->partial_slab_list, a_slab, link);
	} else if (!a_slab->num_busy_obj) {
		// if there are none, move to from partial to empty
		TAILQ_REMOVE(&cp->partial_slab_list, a_slab, link);
		TAILQ_INSERT_HEAD(&cp->empty_slab_list, a_slab, link);
	}
	spin_unlock_irqsave(&cp->cache_lock);
}

void kmem_cache_free(struct kmem_cache *kc, void *buf)
{
	struct kmem_pcpu_cache *pcc = get_my_pcpu_cache(kc);
	struct kmem_depot *depot = &kc->depot;
	struct kmem_magazine *mag;

	lock_pcu_cache(pcc);
try_free:
	if (pcc->loaded->nr_rounds < pcc->magsize) {
		pcc->loaded->rounds[pcc->loaded->nr_rounds] = buf;
		pcc->loaded->nr_rounds++;
		unlock_pcu_cache(pcc);
		return;
	}
	/* The paper checks 'is empty' here.  But we actually just care if it has
	 * room left, not that prev is completely empty.  This could be the case due
	 * to magazine resize. */
	if (pcc->prev->nr_rounds < pcc->magsize) {
		__swap_mags(pcc);
		goto try_free;
	}
	lock_depot(depot);
	/* Here's where the resize magic happens.  We'll start using it for the next
	 * magazine. */
	pcc->magsize = depot->magsize;
	mag = SLIST_FIRST(&depot->empty);
	if (mag) {
		SLIST_REMOVE_HEAD(&depot->empty, link);
		depot->nr_empty--;
		__return_to_depot(kc, pcc->prev);
		unlock_depot(depot);
		pcc->prev = pcc->loaded;
		pcc->loaded = mag;
		goto try_free;
	}
	unlock_depot(depot);
	/* Need to unlock, in case we end up calling back into ourselves. */
	unlock_pcu_cache(pcc);
	/* don't want to wait on a free.  if this fails, we can still just give it
	 * to the slab layer. */
	mag = kmem_cache_alloc(kmem_magazine_cache, MEM_ATOMIC);
	if (mag) {
		assert(mag->nr_rounds == 0);	/* paranoia, can probably remove */
		lock_depot(depot);
		SLIST_INSERT_HEAD(&depot->empty, mag, link);
		depot->nr_empty++;
		unlock_depot(depot);
		lock_pcu_cache(pcc);
		goto try_free;
	}
	if (kc->dtor)
		kc->dtor(buf, kc->priv);
	__kmem_free_to_slab(kc, buf);
}

/* Back end: internal functions */
/* When this returns, the cache has at least one slab in the empty list.  If
 * page_alloc fails, there are some serious issues.  This only grows by one slab
 * at a time.
 *
 * Grab the cache lock before calling this.
 *
 * TODO: think about page colouring issues with kernel memory allocation. */
static bool kmem_cache_grow(struct kmem_cache *cp)
{
	struct kmem_slab *a_slab;
	struct kmem_bufctl *a_bufctl;

	if (!__use_bufctls(cp)) {
		void *a_page;

		/* Careful, this assumes our source is a PGSIZE-aligned allocator.  We
		 * could use xalloc to enforce the alignment, but that'll bypass the
		 * qcaches, which we don't want.  Caller beware. */
		a_page = arena_alloc(cp->source, PGSIZE, MEM_ATOMIC);
		if (!a_page)
			return FALSE;
		// the slab struct is stored at the end of the page
		a_slab = (struct kmem_slab*)(a_page + PGSIZE
		                             - sizeof(struct kmem_slab));
		a_slab->num_busy_obj = 0;
		a_slab->num_total_obj = (PGSIZE - sizeof(struct kmem_slab)) /
		                        cp->obj_size;
		// TODO: consider staggering this IAW section 4.3
		a_slab->free_small_obj = a_page;
		/* Walk and create the free list, which is circular.  Each item stores
		 * the location of the next one at the beginning of the block. */
		void *buf = a_slab->free_small_obj;
		for (int i = 0; i < a_slab->num_total_obj - 1; i++) {
			*(uintptr_t**)buf = buf + cp->obj_size;
			buf += cp->obj_size;
		}
		*((uintptr_t**)buf) = NULL;
	} else {
		void *buf;

		a_slab = kmem_cache_alloc(kmem_slab_cache, 0);
		if (!a_slab)
			return FALSE;
		buf = arena_alloc(cp->source, cp->import_amt, MEM_ATOMIC);
		if (!buf) {
			kmem_cache_free(kmem_slab_cache, a_slab);
			return FALSE;
		}
		a_slab->num_busy_obj = 0;
		a_slab->num_total_obj = cp->import_amt / cp->obj_size;
		BSD_LIST_INIT(&a_slab->bufctl_freelist);
		/* for each buffer, set up a bufctl and point to the buffer */
		for (int i = 0; i < a_slab->num_total_obj; i++) {
			a_bufctl = kmem_cache_alloc(kmem_bufctl_cache, 0);
			BSD_LIST_INSERT_HEAD(&a_slab->bufctl_freelist, a_bufctl, link);
			a_bufctl->buf_addr = buf;
			a_bufctl->my_slab = a_slab;
			buf += cp->obj_size;
		}
	}
	// add a_slab to the empty_list
	TAILQ_INSERT_HEAD(&cp->empty_slab_list, a_slab, link);

	return TRUE;
}

/* This deallocs every slab from the empty list.  TODO: think a bit more about
 * this.  We can do things like not free all of the empty lists to prevent
 * thrashing.  See 3.4 in the paper. */
void kmem_cache_reap(struct kmem_cache *cp)
{
	struct kmem_slab *a_slab, *next;

	// Destroy all empty slabs.  Refer to the notes about the while loop
	spin_lock_irqsave(&cp->cache_lock);
	a_slab = TAILQ_FIRST(&cp->empty_slab_list);
	while (a_slab) {
		next = TAILQ_NEXT(a_slab, link);
		kmem_slab_destroy(cp, a_slab);
		a_slab = next;
	}
	spin_unlock_irqsave(&cp->cache_lock);
}
