/*	$NetBSD: rump.c,v 1.356 2023/08/05 08:05:57 riastradh Exp $	*/

/*
 * Copyright (c) 2007-2011 Antti Kantee.  All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: rump.c,v 1.356 2023/08/05 08:05:57 riastradh Exp $");

#include <sys/systm.h>
#define ELFSIZE ARCH_ELFSIZE

#include <sys/param.h>
#include <sys/atomic.h>
#include <sys/buf.h>
#include <sys/callout.h>
#include <sys/conf.h>
#include <sys/cpu.h>
#include <sys/device.h>
#include <sys/device_impl.h>
#include <sys/evcnt.h>
#include <sys/event.h>
#include <sys/exec_elf.h>
#include <sys/filedesc.h>
#include <sys/iostat.h>
#include <sys/kauth.h>
#include <sys/kcpuset.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/kprintf.h>
#include <sys/kthread.h>
#include <sys/ksyms.h>
#include <sys/msgbuf.h>
#include <sys/module.h>
#include <sys/module_hook.h>
#include <sys/namei.h>
#include <sys/once.h>
#include <sys/percpu.h>
#include <sys/pipe.h>
#include <sys/pool.h>
#include <sys/queue.h>
#include <sys/reboot.h>
#include <sys/resourcevar.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <sys/syscall.h>
#include <sys/syscallvar.h>
#include <sys/threadpool.h>
#include <sys/timetc.h>
#include <sys/tty.h>
#include <sys/uidinfo.h>
#include <sys/vmem.h>
#include <sys/xcall.h>
#include <sys/cprng.h>
#include <sys/rnd.h>
#include <sys/ktrace.h>
#include <sys/pserialize.h>
#include <sys/psref.h>

#include <rump-sys/kern.h>
#include <rump-sys/dev.h>
#include <rump-sys/net.h>
#include <rump-sys/vfs.h>

#include <rump/rumpuser.h>

#include <prop/proplib.h>

#include <uvm/uvm_extern.h>
#include <uvm/uvm_readahead.h>

char machine[] = MACHINE;
char machine_arch[] = MACHINE_ARCH;

struct proc *initproc;

struct device rump_rootdev = {
	.dv_class = DV_VIRTUAL
};

#ifdef RUMP_WITHOUT_THREADS
int rump_threads = 0;
#else
int rump_threads = 1;
#endif

static void rump_component_addlocal(void);
static struct lwp *bootlwp;

/* 16k should be enough for std rump needs */
static  char rump_msgbuf[16*1024] __aligned(256);

bool rump_ttycomponent = false;

extern pool_cache_t pnbuf_cache;

static int rump_inited;

void (*rump_vfs_drainbufs)(int) = (void *)nullop;
int  (*rump_vfs_makeonedevnode)(dev_t, const char *,
				devmajor_t, devminor_t) = (void *)nullop;
int  (*rump_vfs_makedevnodes)(dev_t, const char *, char,
			      devmajor_t, devminor_t, int) = (void *)nullop;
int  (*rump_vfs_makesymlink)(const char *, const char *) = (void *)nullop;

rump_proc_vfs_init_fn rump_proc_vfs_init = (void *)nullop;
rump_proc_vfs_release_fn rump_proc_vfs_release = (void *)nullop;

static void add_linkedin_modules(const struct modinfo *const *, size_t);
static void add_static_evcnt(struct evcnt *);

static pid_t rspo_wrap_getpid(void) {
	return rump_sysproxy_hyp_getpid();
}
static int rspo_wrap_syscall(int num, void *arg, long *retval) {
	return rump_sysproxy_hyp_syscall(num, arg, retval);
}
static int rspo_wrap_rfork(void *priv, int flag, const char *comm) {
	return rump_sysproxy_hyp_rfork(priv, flag, comm);
}
static void rspo_wrap_lwpexit(void) {
	rump_sysproxy_hyp_lwpexit();
}
static void rspo_wrap_execnotify(const char *comm) {
	rump_sysproxy_hyp_execnotify(comm);
}
static const struct rumpuser_hyperup hyp = {
	.hyp_schedule		= rump_schedule,
	.hyp_unschedule		= rump_unschedule,
	.hyp_backend_unschedule	= rump_user_unschedule,
	.hyp_backend_schedule	= rump_user_schedule,
	.hyp_lwproc_switch	= rump_lwproc_switch,
	.hyp_lwproc_release	= rump_lwproc_releaselwp,
	.hyp_lwproc_newlwp	= rump_lwproc_newlwp,
	.hyp_lwproc_curlwp	= rump_lwproc_curlwp,

	.hyp_getpid		= rspo_wrap_getpid,
	.hyp_syscall		= rspo_wrap_syscall,
	.hyp_lwproc_rfork	= rspo_wrap_rfork,
	.hyp_lwpexit		= rspo_wrap_lwpexit,
	.hyp_execnotify		= rspo_wrap_execnotify,
};
struct rump_sysproxy_ops rump_sysproxy_ops = {
	.rspo_copyin		= (void *)enxio,
	.rspo_copyinstr 	= (void *)enxio,
	.rspo_copyout	 	= (void *)enxio,
	.rspo_copyoutstr 	= (void *)enxio,
	.rspo_anonmmap 		= (void *)enxio,
	.rspo_raise 		= (void *)enxio,
	.rspo_fini 		= (void *)enxio,
	.rspo_hyp_getpid 	= (void *)enxio,
	.rspo_hyp_syscall 	= (void *)enxio,
	.rspo_hyp_rfork 	= (void *)enxio,
	.rspo_hyp_lwpexit 	= (void *)enxio,
	.rspo_hyp_execnotify 	= (void *)enxio,
};

int
rump_daemonize_begin(void)
{

	if (rump_inited)
		return EALREADY;

	return rumpuser_daemonize_begin();
}

int
rump_daemonize_done(int error)
{

	return rumpuser_daemonize_done(error);
}

#ifdef RUMP_USE_CTOR

/* sysctl bootstrap handling */
struct sysctl_boot_chain sysctl_boot_chain \
    = LIST_HEAD_INITIALIZER(sysctl_boot_chain);
__link_set_add_text(sysctl_funcs,voidop); /* ensure linkset is non-empty */

#else /* RUMP_USE_CTOR */

RUMP_COMPONENT(RUMP_COMPONENT_POSTINIT)
{
	__link_set_decl(rump_components, struct rump_component);

	/*
	 * Trick compiler into generating references so that statically
	 * linked rump kernels are generated with the link set symbols.
	 */
	asm("" :: "r"(__start_link_set_rump_components));
	asm("" :: "r"(__stop_link_set_rump_components));
}

#endif /* RUMP_USE_CTOR */

int
rump_init_callback(void (*cpuinit_callback) (void))
{
	char buf[256];
	struct timespec bts;
	int64_t sec;
	long nsec;
	struct lwp *l, *initlwp;
	int i, numcpu;

	/* not reentrant */
	if (rump_inited)
		return 0;
	else if (rump_inited == -1)
		panic("%s: host process restart required", __func__);
	else
		rump_inited = 1;

	/* initialize hypervisor */
	if (rumpuser_init(RUMPUSER_VERSION, &hyp) != 0) {
		rumpuser_dprintf("rumpuser init failed\n");
		return EINVAL;
	}

	/* init minimal lwp/cpu context */
	rump_lwproc_init();
	l = &lwp0;
	l->l_cpu = l->l_target_cpu = &rump_bootcpu;
	rump_lwproc_curlwp_set(l);

	/* retrieve env vars which affect the early stage of bootstrap */
	if (rumpuser_getparam("RUMP_THREADS", buf, sizeof(buf)) == 0) {
		rump_threads = *buf != '0';
	}
	if (rumpuser_getparam("RUMP_VERBOSE", buf, sizeof(buf)) == 0) {
		if (*buf != '0')
			boothowto = AB_VERBOSE;
	}

	if (rumpuser_getparam(RUMPUSER_PARAM_NCPU, buf, sizeof(buf)) != 0)
		panic("%s: mandatory hypervisor configuration (NCPU) missing",
		    __func__);
	numcpu = strtoll(buf, NULL, 10);
	if (numcpu < 1) {
		panic("%s: rump kernels are not lightweight enough for %d CPUs",
		    __func__, numcpu);
	}

	rump_thread_init();
	rump_cpus_bootstrap(&numcpu);

	rumpuser_clock_gettime(RUMPUSER_CLOCK_RELWALL, &sec, &nsec);
	bts.tv_sec = sec;
	bts.tv_nsec = nsec;

	initmsgbuf(rump_msgbuf, sizeof(rump_msgbuf));
	aprint_verbose("%s%s", copyright, version);

	rump_intr_init(numcpu);

	rump_tsleep_init();

	rumpuser_mutex_init(&rump_giantlock, RUMPUSER_MTX_SPIN);
	ksyms_init();
	uvm_init();
	evcnt_init();

	kcpuset_sysinit();
	once_init();
	kernconfig_lock_init();
	prop_kern_init();

	kmem_init();

	uvm_ra_init();
	uao_init();

	mutex_obj_init();
	rw_obj_init();
	callout_startup();

	kprintf_init();
	percpu_init();
	pserialize_init();

	kauth_init();

	secmodel_init();
	sysctl_init();
	/*
	 * The above call to sysctl_init() only initializes sysctl nodes
	 * from link sets.  Initialize sysctls in case we used ctors.
	 */
#ifdef RUMP_USE_CTOR
	{
		struct sysctl_setup_chain *ssc;

		while ((ssc = LIST_FIRST(&sysctl_boot_chain)) != NULL) {
			LIST_REMOVE(ssc, ssc_entries);
			ssc->ssc_func(NULL);
		}
	}
#endif /* RUMP_USE_CTOR */

	rnd_init();
	rump_hyperentropy_init();

	procinit();
	proc0_init();
	uid_init();
	chgproccnt(0, 1);

	l->l_proc = &proc0;
	lwp_update_creds(l);

	lwpinit_specificdata();
	lwp_initspecific(&lwp0);

	/* Must be called after lwpinit_specificdata */
	psref_init();

	threadpools_init();

	loginit();

	rump_biglock_init();

	rump_scheduler_init(numcpu);
	/* revert temporary context and schedule a semireal context */
	rump_lwproc_curlwp_clear(l);
	initproc = &proc0; /* borrow proc0 before we get initproc started */
	rump_schedule();
	bootlwp = curlwp;

	inittimecounter();
	ntp_init();

#ifdef KTRACE
	ktrinit();
#endif

	tc_setclock(&bts);

	extern krwlock_t exec_lock;
	rw_init(&exec_lock);

	/* we are mostly go.  do per-cpu subsystem init */
	for (i = 0; i < numcpu; i++) {
		struct cpu_info *ci = cpu_lookup(i);

		/* attach non-bootstrap CPUs */
		if (i > 0) {
			rump_cpu_attach(ci);
			ncpu++;
		}
		snprintf(ci->ci_cpuname, sizeof ci->ci_cpuname, "cpu%d", i);

		callout_init_cpu(ci);
		softint_init(ci);
		xc_init_cpu(ci);
		pool_cache_cpu_init(ci);
		selsysinit(ci);
		percpu_init_cpu(ci);

		TAILQ_INIT(&ci->ci_data.cpu_ld_locks);
		__cpu_simple_lock_init(&ci->ci_data.cpu_ld_lock);

		aprint_verbose("cpu%d at thinair0: rump virtual cpu\n", i);
	}
	ncpuonline = ncpu;

	/* Once all CPUs are detected, initialize the per-CPU cprng_fast.  */
	cprng_init();
	cprng_fast_init();

	if (cpuinit_callback)
		(*cpuinit_callback)();

	rnd_init_softint();

	kqueue_init();
	iostat_init();
	fd_sys_init();
	module_init();
	module_hook_init();
	devsw_init();
	pipe_init();
	resource_init();
	procinit_sysctl();
	time_init();
	config_init();

	/* start page baroness */
	if (rump_threads) {
		if (kthread_create(PRI_PGDAEMON, KTHREAD_MPSAFE, NULL,
		    uvm_pageout, NULL, &uvm.pagedaemon_lwp, "pdaemon") != 0)
			panic("%s: pagedaemon create failed", __func__);
	} else
		uvm.pagedaemon_lwp = NULL; /* doesn't match curlwp */

	/* process dso's */
	rumpuser_dl_bootstrap(add_linkedin_modules,
	    rump_kernelfsym_load, rump_component_load, add_static_evcnt);

	rump_component_addlocal();
	rump_component_init(RUMP_COMPONENT_KERN);

	/* initialize factions, if present */
	rump_component_init(RUMP__FACTION_VFS);
	/* pnbuf_cache is used even without vfs */
	if (rump_component_count(RUMP__FACTION_VFS) == 0) {
		pnbuf_cache = pool_cache_init(MAXPATHLEN, 0, 0, 0, "pnbufpl",
		    NULL, IPL_NONE, NULL, NULL, NULL);
	}
	rump_component_init(RUMP__FACTION_NET);
	rump_component_init(RUMP__FACTION_DEV);
	KASSERT(rump_component_count(RUMP__FACTION_VFS) <= 1
	    && rump_component_count(RUMP__FACTION_NET) <= 1
	    && rump_component_count(RUMP__FACTION_DEV) <= 1);

	rump_component_init(RUMP_COMPONENT_KERN_VFS);

	/*
	 * if we initialized the tty component above, the tyttymtx is
	 * now initialized.  otherwise, we need to initialize it.
	 */
	if (!rump_ttycomponent)
		mutex_init(&tty_lock, MUTEX_DEFAULT, IPL_VM);

	cold = 0;

	mp_online = true;

	/* CPUs are up.  allow kernel threads to run */
	rump_thread_allow(NULL);

	sysctl_finalize();

	module_init_class(MODULE_CLASS_ANY);

	if (rumpuser_getparam(RUMPUSER_PARAM_HOSTNAME,
	    hostname, MAXHOSTNAMELEN) != 0) {
		panic(
		    "%s: mandatory hypervisor configuration (HOSTNAME) missing",
		    __func__);
	}
	hostnamelen = strlen(hostname);

	sigemptyset(&sigcantmask);

	if (rump_threads)
		vmem_rehash_start();

	/*
	 * Create init (proc 1), used to attach implicit threads in rump.
	 * (note: must be done after vfsinit to get cwdi)
	 */
	initlwp = rump__lwproc_alloclwp(NULL);
	mutex_enter(&proc_lock);
	initproc = proc_find_raw(1);
	mutex_exit(&proc_lock);
	if (initproc == NULL)
		panic("%s: where in the world is initproc?", __func__);
	strlcpy(initproc->p_comm, "rumplocal", sizeof(initproc->p_comm));

	rump_component_init(RUMP_COMPONENT_POSTINIT);

	/* load syscalls */
	rump_component_init(RUMP_COMPONENT_SYSCALL);

	/* component inits done */
	bootlwp = NULL;

	/* open 0/1/2 for init */
	KASSERT(rump_lwproc_curlwp() == NULL);
	rump_lwproc_switch(initlwp);
	rump_consdev_init();
	rump_lwproc_switch(NULL);

	/* release cpu */
	rump_unschedule();

	return 0;
}

int
rump_init(void)
{
	return rump_init_callback(NULL);
}

/* historic compat */
__strong_alias(rump__init,rump_init);

static int compcounter[RUMP_COMPONENT_MAX];
static int compinited[RUMP_COMPONENT_MAX];

/*
 * Yea, this is O(n^2), but we're only looking at a handful of components.
 * Components are always initialized from the thread that called rump_init().
 */
static LIST_HEAD(, rump_component) rchead = LIST_HEAD_INITIALIZER(rchead);

#ifdef RUMP_USE_CTOR
struct modinfo_boot_chain modinfo_boot_chain \
    = LIST_HEAD_INITIALIZER(modinfo_boot_chain);

static void
rump_component_addlocal(void)
{
	struct modinfo_chain *mc;
	
	while ((mc = LIST_FIRST(&modinfo_boot_chain)) != NULL) {
		LIST_REMOVE(mc, mc_entries);
		module_builtin_add(&mc->mc_info, 1, false);
	}
}

#else /* RUMP_USE_CTOR */

static void
rump_component_addlocal(void)
{
	__link_set_decl(rump_components, struct rump_component);
	struct rump_component *const *rc;

	__link_set_foreach(rc, rump_components) {
		rump_component_load(*rc);
	}
}
#endif /* RUMP_USE_CTOR */

void
rump_component_load(const struct rump_component *rc_const)
{
	struct rump_component *rc, *rc_iter;

	/* time for rump component loading and unloading has passed */
	if (!cold)
		return;

	/*
	 * XXX: this is ok since the "const" was removed from the
	 * definition of RUMP_COMPONENT().
	 *
	 * However, to preserve the hypercall interface, the const
	 * remains here.  This can be fixed in the next hypercall revision.
	 */
	rc = __UNCONST(rc_const);

	KASSERT(!rump_inited || curlwp == bootlwp);

	LIST_FOREACH(rc_iter, &rchead, rc_entries) {
		if (rc_iter == rc)
			return;
	}

	LIST_INSERT_HEAD(&rchead, rc, rc_entries);
	KASSERT(rc->rc_type < RUMP_COMPONENT_MAX);
	compcounter[rc->rc_type]++;
}

void
rump_component_unload(struct rump_component *rc)
{

	/*
	 * Checking for cold is enough because rump_init() both
	 * flips it and handles component loading.
	 */
	if (!cold)
		return;

	LIST_REMOVE(rc, rc_entries);
}

int
rump_component_count(enum rump_component_type type)
{

	KASSERT(curlwp == bootlwp);
	KASSERT(type < RUMP_COMPONENT_MAX);
	return compcounter[type];
}

void
rump_component_init(enum rump_component_type type)
{
	struct rump_component *rc, *rc_next, rc_marker;

	KASSERT(curlwp == bootlwp);
	KASSERT(!compinited[type]);

	rc_marker.rc_type = RUMP_COMPONENT_MAX;
	rc_marker.rc_init = NULL;
	for (rc = LIST_FIRST(&rchead); rc != NULL; rc = rc_next) {
		if (rc->rc_type == type) {
			LIST_INSERT_AFTER(rc, &rc_marker, rc_entries);
			rc->rc_init();
			LIST_REMOVE(rc, rc_entries);
			rc_next = LIST_NEXT(&rc_marker, rc_entries);
			LIST_REMOVE(&rc_marker, rc_entries);
		} else {
			rc_next = LIST_NEXT(rc, rc_entries);
		}
	}
	compinited[type] = 1;
}

/*
 * Initialize a module which has already been loaded and linked
 * with dlopen(). This is fundamentally the same as a builtin module.
 *
 * XXX: this interface does not really work in the RUMP_USE_CTOR case,
 * but I'm not sure it's anything to cry about.  In feeling blue,
 * things could somehow be handled via modinfo_boot_chain.
 */
int
rump_module_init(const struct modinfo * const *mip, size_t nmodinfo)
{

	return module_builtin_add(mip, nmodinfo, true);
}

/*
 * Finish module (flawless victory, fatality!).
 */
int
rump_module_fini(const struct modinfo *mi)
{

	return module_builtin_remove(mi, true);
}

/*
 * Add loaded and linked module to the builtin list.  It will
 * later be initialized with module_init_class().
 */

static void
add_linkedin_modules(const struct modinfo * const *mip, size_t nmodinfo)
{

	module_builtin_add(mip, nmodinfo, false);
}

/*
 * Add an evcnt.
 */
static void
add_static_evcnt(struct evcnt *ev)
{

	evcnt_attach_static(ev);
}

int
rump_kernelfsym_load(void *symtab, uint64_t symsize,
	char *strtab, uint64_t strsize)
{
	static int inited = 0;
	Elf64_Ehdr ehdr;

	if (inited)
		return EBUSY;
	inited = 1;

	/*
	 * Use 64bit header since it's bigger.  Shouldn't make a
	 * difference, since we're passing in all zeroes anyway.
	 */
	memset(&ehdr, 0, sizeof(ehdr));
	ksyms_addsyms_explicit(&ehdr, symtab, symsize, strtab, strsize);

	return 0;
}

int
rump_boot_gethowto()
{

	return boothowto;
}

void
rump_boot_sethowto(int howto)
{

	boothowto = howto;
}

int
rump_getversion(void)
{

	return __NetBSD_Version__;
}
/* compat */
__strong_alias(rump_pub_getversion,rump_getversion);

/*
 * Note: may be called unscheduled.  Not fully safe since no locking
 * of allevents (currently that's not even available).
 */
void
rump_printevcnts()
{
	struct evcnt *ev;

	TAILQ_FOREACH(ev, &allevents, ev_list)
		rumpuser_dprintf("%s / %s: %" PRIu64 "\n",
		    ev->ev_group, ev->ev_name, ev->ev_count);
}

/*
 * If you use this interface ... well ... all bets are off.
 * The original purpose is for the p2k fs server library to be
 * able to use the same pid/lid for VOPs as the host kernel.
 */
void
rump_allbetsareoff_setid(pid_t pid, int lid)
{
	struct lwp *l = curlwp;
	struct proc *p = l->l_proc;

	l->l_lid = lid;
	p->p_pid = pid;
}

static void
ipiemu(void *a1, void *a2)
{

	xc__highpri_intr(NULL);
}

void
rump_xc_highpri(struct cpu_info *ci)
{

	if (ci)
		xc_unicast(0, ipiemu, NULL, NULL, ci);
	else
		xc_broadcast(0, ipiemu, NULL, NULL);
}

int
rump_syscall(int num, void *data, size_t dlen, register_t *retval)
{
	struct proc *p;
	struct emul *e;
	struct sysent *callp;
	const int *etrans = NULL;
	int rv;

	rump_schedule();
	p = curproc;
	e = p->p_emul;
#ifndef __HAVE_MINIMAL_EMUL
	num &= e->e_nsysent - 1;
#else
	num &= SYS_NSYSENT - 1;
#endif
	callp = e->e_sysent + num;

	rv = sy_invoke(callp, curlwp, data, retval, num);

	/*
	 * I hope that (!__HAVE_MINIMAL_EMUL || __HAVE_SYSCALL_INTERN) is
	 * an invariant ...
	 */
#if !defined(__HAVE_MINIMAL_EMUL)
	etrans = e->e_errno;
#elif defined(__HAVE_SYSCALL_INTERN)
	etrans = p->p_emuldata;
#endif

	if (etrans) {
		rv = etrans[rv];
		/*
		 * XXX: small hack since Linux etrans vectors on some
		 * archs contain negative errnos, but rump_syscalls
		 * uses the -1 + errno ABI.  Note that these
		 * negative values are always the result of translation,
		 * otherwise the above translation method would not
		 * work very well.
		 */
		if (rv < 0)
			rv = -rv;
	}
	rump_unschedule();

	return rv;
}

void
rump_syscall_boot_establish(const struct rump_onesyscall *calls, size_t ncall)
{
	struct sysent *callp;
	size_t i;

	for (i = 0; i < ncall; i++) {
		callp = rump_sysent + calls[i].ros_num;
		KASSERT(bootlwp != NULL
		    && callp->sy_call == (sy_call_t *)(void *)enosys);
		callp->sy_call = calls[i].ros_handler;
	}
}

struct rump_boot_etfs *ebstart;
void
rump_boot_etfs_register(struct rump_boot_etfs *eb)
{

	/*
	 * Could use atomics, but, since caller would need to synchronize
	 * against calling rump_init() anyway, easier to just specify the
	 * interface as "caller serializes".  This solve-by-specification
	 * approach avoids the grey area of using atomics before rump_init()
	 * runs.
	 */
	eb->_eb_next = ebstart;
	eb->eb_status = -1;
	ebstart = eb;
}
