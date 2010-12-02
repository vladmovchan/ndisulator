/*-
 * Copyright (c) 2005
 *      Bill Paul <wpaul@windriver.com>.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *      This product includes software developed by Bill Paul.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Bill Paul AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL Bill Paul OR THE VOICES IN HIS HEAD
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/unistd.h>

#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/module.h>
#include <sys/conf.h>
#include <sys/mbuf.h>
#include <sys/bus.h>
#include <sys/proc.h>
#include <sys/sched.h>
#include <sys/smp.h>
#include <sys/queue.h>

#ifdef __i386__
#include <machine/segments.h>
#endif
#ifdef __amd64__
#include <machine/fpu.h>
#endif

#include <dev/usb/usb.h>

#include <compat/ndis/pe_var.h>
#include <compat/ndis/resource_var.h>
#include <compat/ndis/ntoskrnl_var.h>
#include <compat/ndis/ndis_var.h>
#include <compat/ndis/hal_var.h>
#include <compat/ndis/usbd_var.h>

static struct mtx drvdb_mtx;
static STAILQ_HEAD(drvdb, drvdb_ent) drvdb_head;

static struct driver_object fake_pci_driver; /* serves both PCI and cardbus */
static struct driver_object fake_pccard_driver;

#ifdef __i386__
static void x86_oldldt(void *);
static void x86_newldt(void *);

struct tid {
	void		*tid_except_list;	/* 0x00 */
	uint32_t	tid_oldfs;		/* 0x04 */
	uint32_t	tid_selector;		/* 0x08 */
	struct tid	*tid_self;		/* 0x0C */
};
static struct tid *my_tids;
#endif /* __i386__ */

MALLOC_DEFINE(M_NDIS_WINDRV, "ndis_windrv", "ndis_windrv buffers");

#define	DUMMY_REGISTRY_PATH "\\\\some\\bogus\\path"

void
windrv_libinit(void)
{
	STAILQ_INIT(&drvdb_head);
	mtx_init(&drvdb_mtx, "Windows driver DB lock",
	    "Windows internal lock", MTX_DEF);

	/*
	 * PCI and pccard devices don't need to use IRPs to
	 * interact with their bus drivers (usually), so our
	 * emulated PCI and pccard drivers are just stubs.
	 * USB devices, on the other hand, do all their I/O
	 * by exchanging IRPs with the USB bus driver, so
	 * for that we need to provide emulator dispatcher
	 * routines, which are in a separate module.
	 */
	windrv_bus_attach(&fake_pci_driver, "PCI Bus");
	windrv_bus_attach(&fake_pccard_driver, "PCCARD Bus");
#ifdef __i386__
	/*
	 * In order to properly support SMP machines, we have
	 * to modify the GDT on each CPU, since we never know
	 * on which one we'll end up running.
	 */
	my_tids = malloc(sizeof(struct tid) * mp_ncpus,
	    M_NDIS_WINDRV, M_NOWAIT|M_ZERO);
	if (my_tids == NULL)
		panic("failed to allocate thread info blocks");
	smp_rendezvous(NULL, x86_newldt, NULL, NULL);
#endif
}

void
windrv_libfini(void)
{
	struct drvdb_ent *d;

	mtx_lock(&drvdb_mtx);
	while (STAILQ_FIRST(&drvdb_head) != NULL) {
		d = STAILQ_FIRST(&drvdb_head);
		STAILQ_REMOVE_HEAD(&drvdb_head, link);
		free(d, M_NDIS_WINDRV);
	}
	mtx_unlock(&drvdb_mtx);

	RtlFreeUnicodeString(&fake_pci_driver.driver_name);
	RtlFreeUnicodeString(&fake_pccard_driver.driver_name);

	mtx_destroy(&drvdb_mtx);
#ifdef __i386__
	smp_rendezvous(NULL, x86_oldldt, NULL, NULL);
	free(my_tids, M_NDIS_WINDRV);
#endif
}

/*
 * Given the address of a driver image, find its corresponding driver_object.
 */
struct driver_object *
windrv_lookup(vm_offset_t img, const char *name)
{
	struct drvdb_ent *d;
	unicode_string us;
	ansi_string as;

	bzero((char *)&us, sizeof(us));

	/* Damn unicode. */
	if (name != NULL) {
		RtlInitAnsiString(&as, name);
		if (RtlAnsiStringToUnicodeString(&us, &as, TRUE))
			return (NULL);
	}

	mtx_lock(&drvdb_mtx);
	STAILQ_FOREACH(d, &drvdb_head, link) {
		if (d->windrv_object->driver_start == (void *)img ||
		    (bcmp((char *)d->windrv_object->driver_name.us_buf,
		    (char *)us.us_buf, us.us_len) == 0 && us.us_len)) {
			mtx_unlock(&drvdb_mtx);
			if (name != NULL)
				RtlFreeUnicodeString(&us);
			return (d->windrv_object);
		}
	}
	mtx_unlock(&drvdb_mtx);

	if (name != NULL)
		RtlFreeUnicodeString(&us);

	return (NULL);
}

struct drvdb_ent *
windrv_match(matchfuncptr matchfunc, void *ctx)
{
	struct drvdb_ent *d;

	mtx_lock(&drvdb_mtx);
	STAILQ_FOREACH(d, &drvdb_head, link) {
		if (d->windrv_devlist == NULL)
			continue;
		if (matchfunc(d->windrv_bustype, d->windrv_devlist, ctx)) {
			mtx_unlock(&drvdb_mtx);
			return (d);
		}
	}
	mtx_unlock(&drvdb_mtx);

	return (NULL);
}

/*
 * Remove a driver_object from our datatabase and destroy it. Throw
 * away any custom driver extension info that may have been added.
 */
int
windrv_unload(module_t mod, vm_offset_t img)
{
	struct drvdb_ent *db, *r = NULL;
	struct driver_object *drv;
	struct device_object *pdo;
	device_t dev;
	struct list_entry *e;

	drv = windrv_lookup(img, NULL);

	/*
	 * When we unload a driver image, we need to force a
	 * detach of any devices that might be using it. We
	 * need the PDOs of all attached devices for this.
	 * Getting at them is a little hard. We basically
	 * have to walk the device lists of all our bus
	 * drivers.
	 */
	mtx_lock(&drvdb_mtx);
	STAILQ_FOREACH(db, &drvdb_head, link) {
		/*
		 * Fake bus drivers have no devlist info.
		 * If this driver has devlist info, it's
		 * a loaded Windows driver and has no PDOs,
		 * so skip it.
		 */
		if (db->windrv_devlist != NULL)
			continue;
		pdo = db->windrv_object->device_object;
		while (pdo != NULL) {
			if (pdo->attacheddev->drvobj != drv) {
				pdo = pdo->nextdev;
				continue;
			}
			dev = pdo->devext;
			pdo = pdo->nextdev;
			mtx_unlock(&drvdb_mtx);
			device_detach(dev);
			mtx_lock(&drvdb_mtx);
		}
	}

	STAILQ_FOREACH(db, &drvdb_head, link) {
		if (db->windrv_object->driver_start == (void *)img) {
			r = db;
			STAILQ_REMOVE(&drvdb_head, db, drvdb_ent, link);
			break;
		}
	}
	mtx_unlock(&drvdb_mtx);

	if (r == NULL || drv == NULL)
		return (ENOENT);

	/* Destroy any custom extensions that may have been added. */
	drv = r->windrv_object;
	while (!IsListEmpty(&drv->driver_extension->usrext)) {
		e = RemoveHeadList(&drv->driver_extension->usrext);
		ExFreePool(e);
	}

	free(drv->driver_extension, M_NDIS_WINDRV);
	RtlFreeUnicodeString(&drv->driver_name);
	free(drv, M_NDIS_WINDRV);
	free(r, M_NDIS_WINDRV);		/* Free our DB handle */

	return (0);
}

#define	WINDRV_LOADED		htonl(0x42534F44)

#ifdef __amd64__
static void
patch_user_shared_data_address(vm_offset_t img, size_t len)
{
	unsigned long i, n, max_addr, *addr;

	n = len - sizeof(unsigned long);
	max_addr = KI_USER_SHARED_DATA + sizeof(kuser_shared_data);
	for (i = 0; i < n; i++) {
		addr = (unsigned long *)(img + i);
		if (*addr >= KI_USER_SHARED_DATA && *addr < max_addr) {
			*addr -= KI_USER_SHARED_DATA;
			*addr += (unsigned long)&kuser_shared_data;
		}
	}
}
#endif

/*
 * Loader routine for actual Windows driver modules, ultimately
 * calls the driver's DriverEntry() routine.
 */
int
windrv_load(module_t mod, vm_offset_t img, size_t len,
    uint32_t bustype, void *devlist, void *regvals)
{
	struct image_optional_header *opt_hdr;
	driver_entry entry;
	struct drvdb_ent *new;
	struct driver_object *drv;
	uint32_t *ptr;
	int32_t rval;
	ansi_string as;

	if (pe_validate_header(img))
		return (ENOEXEC);
	/*
	 * First step: try to relocate and dynalink the executable
	 * driver image.
	 */
	ptr = (uint32_t *)(img + 8);
	if (*ptr == WINDRV_LOADED)
		goto skipreloc;

	/* Perform text relocation */
	if (pe_relocate(img))
		return (ENOEXEC);

	/* Dynamically link the NDIS.SYS routines -- required. */
	if (pe_patch_imports(img, "NDIS", ndis_functbl))
		return (ENOEXEC);

	/* Dynamically link the HAL.dll routines -- optional. */
	pe_patch_imports(img, "HAL", hal_functbl);

	/* Dynamically link ntoskrnl.exe -- optional. */
	pe_patch_imports(img, "ntoskrnl", ntoskrnl_functbl);

	/* Dynamically link USBD.SYS -- optional */
	pe_patch_imports(img, "USBD", usbd_functbl);
#ifdef __amd64__
	patch_user_shared_data_address(img, len);
#endif
	*ptr = WINDRV_LOADED;
skipreloc:
	/* Next step: find the driver entry point. */
	pe_get_optional_header(img, &opt_hdr);
	entry = (driver_entry)pe_translate_addr(img,
	    opt_hdr->address_of_entry_point);

	/* Next step: allocate and store a driver object. */
	new = malloc(sizeof(struct drvdb_ent), M_NDIS_WINDRV, M_NOWAIT|M_ZERO);
	if (new == NULL)
		return (ENOMEM);

	drv = malloc(sizeof(struct driver_object), M_NDIS_WINDRV,
	    M_NOWAIT|M_ZERO);
	if (drv == NULL) {
		free (new, M_NDIS_WINDRV);
		return (ENOMEM);
	}

	/* Allocate a driver extension structure too. */
	drv->driver_extension = malloc(sizeof(struct driver_extension),
	    M_NDIS_WINDRV, M_NOWAIT|M_ZERO);
	if (drv->driver_extension == NULL) {
		free(new, M_NDIS_WINDRV);
		free(drv, M_NDIS_WINDRV);
		return (ENOMEM);
	}

	InitializeListHead((&drv->driver_extension->usrext));

	drv->driver_start = (void *)img;
	drv->driver_size = len;

	RtlInitAnsiString(&as, DUMMY_REGISTRY_PATH);
	if (RtlAnsiStringToUnicodeString(&drv->driver_name, &as, TRUE)) {
		free(drv->driver_extension, M_NDIS_WINDRV);
		free(drv, M_NDIS_WINDRV);
		free(new, M_NDIS_WINDRV);
		return (ENOMEM);
	}

	/* Now call the DriverEntry() function. */
	rval = MSCALL2(entry, drv, &drv->driver_name);
	if (rval) {
		RtlFreeUnicodeString(&drv->driver_name);
		free(drv->driver_extension, M_NDIS_WINDRV);
		free(drv, M_NDIS_WINDRV);
		free(new, M_NDIS_WINDRV);
		printf("NDIS: driver entry failed; status: 0x%08X\n", rval);
		return (ENODEV);
	}

	new->windrv_object = drv;
	new->windrv_regvals = regvals;
	new->windrv_devlist = devlist;
	new->windrv_bustype = bustype;

	mtx_lock(&drvdb_mtx);
	STAILQ_INSERT_HEAD(&drvdb_head, new, link);
	mtx_unlock(&drvdb_mtx);

	return (0);
}

/*
 * Make a new Physical Device Object for a device that was detected/plugged in.
 * For us, the PDO is just a way to get at the device_t.
 */
int32_t
windrv_create_pdo(struct driver_object *drv, device_t bsddev)
{
	struct device_object *dev;
	int32_t rval;

	/*
	 * This is a new physical device object, which technically
	 * is the "top of the stack." Consequently, we don't do
	 * an IoAttachDeviceToDeviceStack() here.
	 */
	mtx_lock(&drvdb_mtx);
	rval = IoCreateDevice(drv, 0, NULL, FILE_DEVICE_UNKNOWN, 0, FALSE, &dev);
	mtx_unlock(&drvdb_mtx);

	if (rval)
		return (rval);
	dev->devext = bsddev;	/* Stash pointer to our BSD device handle. */
	return (NDIS_STATUS_SUCCESS);
}

void
windrv_destroy_pdo(struct driver_object *drv, device_t bsddev)
{
	struct device_object *pdo;

	pdo = windrv_find_pdo(drv, bsddev);
	if (pdo == NULL)
		return;
	pdo->devext = NULL;

	mtx_lock(&drvdb_mtx);
	IoDeleteDevice(pdo);
	mtx_unlock(&drvdb_mtx);
}

/*
 * Given a device_t, find the corresponding PDO in a driver's device list.
 */
struct device_object *
windrv_find_pdo(const struct driver_object *drv, device_t bsddev)
{
	struct device_object *pdo;

	mtx_lock(&drvdb_mtx);
	pdo = drv->device_object;
	while (pdo != NULL) {
		if (pdo->devext == bsddev) {
			mtx_unlock(&drvdb_mtx);
			return (pdo);
		}
		pdo = pdo->nextdev;
	}
	mtx_unlock(&drvdb_mtx);

	return (NULL);
}

/*
 * Add an internally emulated driver to the database. We need this
 * to set up an emulated bus driver so that it can receive IRPs.
 */
int
windrv_bus_attach(struct driver_object *drv, const char *name)
{
	struct drvdb_ent *new;
	ansi_string as;

	new = malloc(sizeof(struct drvdb_ent), M_NDIS_WINDRV, M_NOWAIT|M_ZERO);
	if (new == NULL)
		return (ENOMEM);

	RtlInitAnsiString(&as, name);
	if (RtlAnsiStringToUnicodeString(&drv->driver_name, &as, TRUE)) {
		free(new, M_NDIS_WINDRV);
		return (ENOMEM);
	}

	/*
	 * Set up a fake image pointer to avoid false matches
	 * in windrv_lookup().
	 */
	drv->driver_start = (void *)0xFFFFFFFF;

	new->windrv_object = drv;
	new->windrv_devlist = NULL;
	new->windrv_regvals = NULL;

	mtx_lock(&drvdb_mtx);
	STAILQ_INSERT_HEAD(&drvdb_head, new, link);
	mtx_unlock(&drvdb_mtx);

	return (0);
}

#ifdef __amd64__
extern void x86_64_wrap(void);
extern void x86_64_wrap_call(void);
extern void x86_64_wrap_end(void);

void
windrv_wrap(funcptr func, funcptr *wrap, uint8_t argcnt, uint8_t ftype)
{
	vm_offset_t *calladdr, wrapstart, wrapend, wrapcall;
	funcptr p;

	wrapstart = (vm_offset_t)&x86_64_wrap;
	wrapend = (vm_offset_t)&x86_64_wrap_end;
	wrapcall = (vm_offset_t)&x86_64_wrap_call;

	/* Allocate a new wrapper instance. */
	p = malloc((wrapend - wrapstart), M_NDIS_WINDRV, M_NOWAIT|M_ZERO);
	if (p == NULL)
		panic("failed to allocate new wrapper instance");

	/* Copy over the code. */
	bcopy((char *)wrapstart, p, (wrapend - wrapstart));

	/* Insert the function address into the new wrapper instance. */
	calladdr = (vm_offset_t *)((char *)p + (wrapcall - wrapstart) + 2);
	*calladdr = (vm_offset_t)func;

	*wrap = p;
}

uint64_t
_x86_64_call1(void *fn, uint64_t a)
{
	struct fpu_kern_ctx fpu_ctx_save;
	uint64_t rval;

	fpu_kern_enter(curthread, &fpu_ctx_save, FPU_KERN_NORMAL);
	rval = x86_64_call1(fn, a);
	fpu_kern_leave(curthread, &fpu_ctx_save);

	return (rval);
}

uint64_t
_x86_64_call2(void *fn, uint64_t a, uint64_t b)
{
	struct fpu_kern_ctx fpu_ctx_save;
	uint64_t rval;

	fpu_kern_enter(curthread, &fpu_ctx_save, FPU_KERN_NORMAL);
	rval = x86_64_call2(fn, a, b);
	fpu_kern_leave(curthread, &fpu_ctx_save);

	return (rval);
}

uint64_t
_x86_64_call3(void *fn, uint64_t a, uint64_t b, uint64_t c)
{
	struct fpu_kern_ctx fpu_ctx_save;
	uint64_t rval;

	fpu_kern_enter(curthread, &fpu_ctx_save, FPU_KERN_NORMAL);
	rval = x86_64_call3(fn, a, b, c);
	fpu_kern_leave(curthread, &fpu_ctx_save);

	return (rval);
}

uint64_t
_x86_64_call4(void *fn, uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
	struct fpu_kern_ctx fpu_ctx_save;
	uint64_t rval;

	fpu_kern_enter(curthread, &fpu_ctx_save, FPU_KERN_NORMAL);
	rval = x86_64_call4(fn, a, b, c, d);
	fpu_kern_leave(curthread, &fpu_ctx_save);

	return (rval);
}

uint64_t
_x86_64_call5(void *fn, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
    uint64_t e)
{
	struct fpu_kern_ctx fpu_ctx_save;
	uint64_t rval;

	fpu_kern_enter(curthread, &fpu_ctx_save, FPU_KERN_NORMAL);
	rval = x86_64_call5(fn, a, b, c, d, e);
	fpu_kern_leave(curthread, &fpu_ctx_save);

	return (rval);
}

uint64_t
_x86_64_call6(void *fn, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
    uint64_t e, uint64_t f)
{
	struct fpu_kern_ctx fpu_ctx_save;
	uint64_t rval;

	fpu_kern_enter(curthread, &fpu_ctx_save, FPU_KERN_NORMAL);
	rval = x86_64_call6(fn, a, b, c, d, e, f);
	fpu_kern_leave(curthread, &fpu_ctx_save);

	return (rval);
}
#endif /* __amd64__ */


#ifdef __i386__
struct x86desc {
	uint16_t	x_lolimit;
	uint16_t	x_base0;
	uint8_t		x_base1;
	uint8_t		x_flags;
	uint8_t		x_hilimit;
	uint8_t		x_base2;
};

struct gdt {
	uint16_t		limit;
	void			*base;
} __attribute__((__packed__));

extern uint16_t x86_getfs(void);
extern void x86_setfs(uint16_t);
extern void *x86_gettid(void);
extern void x86_getldt(struct gdt *, uint16_t *);
extern void x86_setldt(struct gdt *, uint16_t);

#define	SEL_LDT	4		/* local descriptor table */
#define	SEL_TO_FS(x)		(((x) << 3))

/*
 * FreeBSD has a special GDT segment reserved specifically for us.
 */
#define	FREEBSD_EMPTYSEL	GNDIS_SEL

/*
 * The meanings of various bits in a descriptor vary a little
 * depending on whether the descriptor will be used as a
 * code, data or system descriptor. (And that in turn depends
 * on which segment register selects the descriptor.)
 * We're only trying to create a data segment, so the definitions
 * below are the ones that apply to a data descriptor.
 */
#define	SEGFLAGLO_PRESENT	0x80	/* segment is present */
#define	SEGFLAGLO_PRIVLVL	0x60	/* privlevel needed for this seg */
#define	SEGFLAGLO_CD		0x10	/* 1 = code/data, 0 = system */
#define	SEGFLAGLO_MBZ		0x08	/* must be zero */
#define	SEGFLAGLO_EXPANDDOWN	0x04	/* limit expands down */
#define	SEGFLAGLO_WRITEABLE	0x02	/* segment is writeable */
#define	SEGGLAGLO_ACCESSED	0x01	/* segment has been accessed */

#define	SEGFLAGHI_GRAN		0x80	/* granularity, 1 = byte, 0 = page */
#define	SEGFLAGHI_BIG		0x40	/* 1 = 32 bit stack, 0 = 16 bit */

/*
 * Context switch from UNIX to Windows. Save the existing value of %fs
 * for this processor, then change it to point to our fake TID.
 */
void
ctxsw_utow(void)
{
	struct tid *t;

	sched_pin();
	critical_enter();
	t = &my_tids[curthread->td_oncpu];
	/*
	 * Ugly hack. During system bootstrap (cold == 1), only CPU 0
	 * is running. So if we were loaded at bootstrap, only CPU 0
	 * will have our special GDT entry. This is a problem for SMP
	 * systems, so to deal with this, we check here to make sure
	 * the TID for this processor has been initialized, and if it
	 * hasn't, we need to do it right now or else things will explode.
	 */
	if (t->tid_self != t)
		x86_newldt(NULL);
	t->tid_oldfs = x86_getfs();
	x86_setfs(SEL_TO_FS(t->tid_selector));

	/* Now entering Windows land, population: you. */
}

/*
 * Context switch from Windows back to UNIX. Restore %fs to its
 * previous value. This always occurs after a call to ctxsw_utow().
 */
void
ctxsw_wtou(void)
{
	struct tid *t;

	t = x86_gettid();
	x86_setfs(t->tid_oldfs);
	critical_exit();
	sched_unpin();

	/* Welcome back to UNIX land, we missed you. */
}

static void windrv_wrap_fastcall(funcptr, funcptr *, uint8_t);
static void windrv_wrap_stdcall(funcptr, funcptr *, uint8_t);
static void windrv_wrap_regparm(funcptr, funcptr *);

extern void x86_fastcall_wrap(void);
extern void x86_fastcall_wrap_arg(void);
extern void x86_fastcall_wrap_call(void);
extern void x86_fastcall_wrap_end(void);

static void
windrv_wrap_fastcall(funcptr func, funcptr *wrap, uint8_t argcnt)
{
	vm_offset_t *calladdr, wrapstart, wrapend, wrapcall, wraparg;
	funcptr p;
	uint8_t *argaddr;

	wrapstart = (vm_offset_t)&x86_fastcall_wrap;
	wrapend = (vm_offset_t)&x86_fastcall_wrap_end;
	wrapcall = (vm_offset_t)&x86_fastcall_wrap_call;
	wraparg = (vm_offset_t)&x86_fastcall_wrap_arg;

	/* Allocate a new wrapper instance. */
	p = malloc((wrapend - wrapstart), M_NDIS_WINDRV, M_NOWAIT|M_ZERO);
	if (p == NULL)
		panic("failed to allocate new wrapper instance");

	/* Copy over the code. */
	bcopy((char *)wrapstart, p, (wrapend - wrapstart));

	/* Insert the function address into the new wrapper instance. */
	calladdr = (vm_offset_t *)((char *)p + ((wrapcall - wrapstart) + 1));
	*calladdr = (vm_offset_t)func;

	if (argcnt < 3)
		argcnt = 0;
	else
		argcnt -= 2;

	argaddr = (uint8_t *)((char *)p + ((wraparg - wrapstart) + 1));
	*argaddr = argcnt * sizeof(uint32_t);

	*wrap = p;
}

extern void x86_stdcall_wrap(void);
extern void x86_stdcall_wrap_call(void);
extern void x86_stdcall_wrap_arg(void);
extern void x86_stdcall_wrap_end(void);

static void
windrv_wrap_stdcall(funcptr func, funcptr *wrap, uint8_t argcnt)
{
	vm_offset_t *calladdr, wrapstart, wrapend, wrapcall, wraparg;
	funcptr p;
	uint8_t *argaddr;

	wrapstart = (vm_offset_t)&x86_stdcall_wrap;
	wrapend = (vm_offset_t)&x86_stdcall_wrap_end;
	wrapcall = (vm_offset_t)&x86_stdcall_wrap_call;
	wraparg = (vm_offset_t)&x86_stdcall_wrap_arg;

	/* Allocate a new wrapper instance. */
	p = malloc((wrapend - wrapstart), M_NDIS_WINDRV, M_NOWAIT|M_ZERO);
	if (p == NULL)
		panic("failed to allocate new wrapper instance");

	/* Copy over the code. */
	bcopy((char *)wrapstart, p, (wrapend - wrapstart));

	/* Insert the function address into the new wrapper instance. */
	calladdr = (vm_offset_t *)((char *)p + ((wrapcall - wrapstart) + 1));
	*calladdr = (vm_offset_t)func;

	argaddr = (uint8_t *)((char *)p + ((wraparg - wrapstart) + 1));
	*argaddr = argcnt * sizeof(uint32_t);

	*wrap = p;
}

extern void x86_regparm_wrap(void);
extern void x86_regparm_wrap_call(void);
extern void x86_regparm_wrap_end(void);

static void
windrv_wrap_regparm(funcptr func, funcptr *wrap)
{
	funcptr p;
	vm_offset_t *calladdr, wrapstart, wrapend, wrapcall;

	wrapstart = (vm_offset_t)&x86_regparm_wrap;
	wrapend = (vm_offset_t)&x86_regparm_wrap_end;
	wrapcall = (vm_offset_t)&x86_regparm_wrap_call;

	/* Allocate a new wrapper instance. */
	p = malloc((wrapend - wrapstart), M_NDIS_WINDRV, M_NOWAIT|M_ZERO);
	if (p == NULL)
		panic("failed to allocate new wrapper instance");

	/* Copy over the code. */
	bcopy((char *)wrapstart, p, (wrapend - wrapstart));

	/* Insert the function address into the new wrapper instance. */
	calladdr = (vm_offset_t *)((char *)p + ((wrapcall - wrapstart) + 1));
	*calladdr = (vm_offset_t)func;

	*wrap = p;
}

void
windrv_wrap(funcptr func, funcptr *wrap, uint8_t argcnt, uint8_t ftype)
{
	switch (ftype) {
	case WINDRV_WRAP_FASTCALL:
		windrv_wrap_fastcall(func, wrap, argcnt);
		break;
	case WINDRV_WRAP_STDCALL:
		windrv_wrap_stdcall(func, wrap, argcnt);
		break;
	case WINDRV_WRAP_REGPARM:
		windrv_wrap_regparm(func, wrap);
		break;
	case WINDRV_WRAP_CDECL:
		windrv_wrap_stdcall(func, wrap, 0);
		break;
	default:
		break;
	}
}

static void
x86_oldldt(void *dummy)
{
	struct x86desc *gdt;
	struct gdt gtable;
	uint16_t ltable;

	mtx_lock_spin(&dt_lock);

	/* Grab location of existing GDT. */
	x86_getldt(&gtable, &ltable);

	/* Find the slot we updated. */
	gdt = gtable.base;
	gdt += FREEBSD_EMPTYSEL;

	/* Empty it out. */
	bzero((char *)gdt, sizeof(struct x86desc));

	/* Restore GDT. */
	x86_setldt(&gtable, ltable);

	mtx_unlock_spin(&dt_lock);
}

static void
x86_newldt(void *dummy)
{
	struct gdt gtable;
	uint16_t ltable;
	struct x86desc *l;
	struct thread *t;

	t = curthread;

	mtx_lock_spin(&dt_lock);

	/* Grab location of existing GDT. */
	x86_getldt(&gtable, &ltable);

	/* Get pointer to the GDT table. */
	l = gtable.base;

	/* Get pointer to empty slot */
	l += FREEBSD_EMPTYSEL;

	/* Initialize TID for this CPU. */
	my_tids[t->td_oncpu].tid_selector = FREEBSD_EMPTYSEL;
	my_tids[t->td_oncpu].tid_self = &my_tids[t->td_oncpu];

	/* Set up new GDT entry. */
	l->x_lolimit = sizeof(struct tid);
	l->x_hilimit = SEGFLAGHI_GRAN|SEGFLAGHI_BIG;
	l->x_base0 = (vm_offset_t)(&my_tids[t->td_oncpu]) & 0xFFFF;
	l->x_base1 = ((vm_offset_t)(&my_tids[t->td_oncpu]) >> 16) & 0xFF;
	l->x_base2 = ((vm_offset_t)(&my_tids[t->td_oncpu]) >> 24) & 0xFF;
	l->x_flags = SEGFLAGLO_PRESENT|SEGFLAGLO_CD|SEGFLAGLO_WRITEABLE;

	/* Update the GDT. */
	x86_setldt(&gtable, ltable);

	mtx_unlock_spin(&dt_lock);

	/* Whew. */
}
#endif /* __i386__ */

void
windrv_unwrap(funcptr func)
{
	free(func, M_NDIS_WINDRV);
}

void
windrv_wrap_table(struct image_patch_table *table)
{
	struct image_patch_table *p;

	for (p = table; p->func != NULL; p++)
		windrv_wrap(p->func, &p->wrap, p->argcnt, p->ftype);
}

void
windrv_unwrap_table(struct image_patch_table *table)
{
	struct image_patch_table *p;

	for (p = table; p->func != NULL; p++)
		windrv_unwrap(p->wrap);
}
