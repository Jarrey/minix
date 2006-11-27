/* This file contains the device dependent part of the drivers for the
 * following special files:
 *     /dev/ram		- RAM disk 
 *     /dev/mem		- absolute memory
 *     /dev/kmem	- kernel virtual memory
 *     /dev/null	- null device (data sink)
 *     /dev/boot	- boot device loaded from boot image 
 *     /dev/zero	- null byte stream generator
 *
 *  Changes:
 *	Apr 29, 2005	added null byte generator  (Jorrit N. Herder)
 *	Apr 09, 2005	added support for boot device  (Jorrit N. Herder)
 *	Jul 26, 2004	moved RAM driver to user-space  (Jorrit N. Herder)
 *	Apr 20, 1992	device dependent/independent split  (Kees J. Bot)
 */

#include "../drivers.h"
#include "../libdriver/driver.h"
#include <sys/ioc_memory.h>
#include <minix/ds.h>
#include "../../kernel/const.h"
#include "../../kernel/config.h"
#include "../../kernel/type.h"

#define MY_DS_NAME_BASE "dev:memory:ramdisk_base"
#define MY_DS_NAME_SIZE "dev:memory:ramdisk_size"

#include <sys/vm.h>

#include "assert.h"

#include "local.h"

#define NR_DEVS            7		/* number of minor devices */

PRIVATE struct device m_geom[NR_DEVS];  /* base and size of each device */
PRIVATE int m_seg[NR_DEVS];  		/* segment index of each device */
PRIVATE int m_device;			/* current device */
PRIVATE struct kinfo kinfo;		/* kernel information */ 
PRIVATE struct machine machine;		/* machine information */ 

extern int errno;			/* error number for PM calls */

FORWARD _PROTOTYPE( char *m_name, (void) 				);
FORWARD _PROTOTYPE( struct device *m_prepare, (int device) 		);
FORWARD _PROTOTYPE( int m_transfer, (int proc_nr, int opcode, u64_t position,
				iovec_t *iov, unsigned nr_req, int safe));
FORWARD _PROTOTYPE( int m_do_open, (struct driver *dp, message *m_ptr) 	);
FORWARD _PROTOTYPE( void m_init, (void) );
FORWARD _PROTOTYPE( int m_ioctl, (struct driver *dp, message *m_ptr, int safe));
FORWARD _PROTOTYPE( void m_geometry, (struct partition *entry) 		);

/* Entry points to this driver. */
PRIVATE struct driver m_dtab = {
  m_name,	/* current device's name */
  m_do_open,	/* open or mount */
  do_nop,	/* nothing on a close */
  m_ioctl,	/* specify ram disk geometry */
  m_prepare,	/* prepare for I/O on a given minor device */
  m_transfer,	/* do the I/O */
  nop_cleanup,	/* no need to clean up */
  m_geometry,	/* memory device "geometry" */
  nop_signal,	/* system signals */
  nop_alarm,
  nop_cancel,
  nop_select,
  NULL,
  NULL
};

/* Buffer for the /dev/zero null byte feed. */
#define ZERO_BUF_SIZE 			1024
PRIVATE char dev_zero[ZERO_BUF_SIZE];

#define click_to_round_k(n) \
	((unsigned) ((((unsigned long) (n) << CLICK_SHIFT) + 512) / 1024))

/*===========================================================================*
 *				   main 				     *
 *===========================================================================*/
PUBLIC int main(void)
{
/* Main program. Initialize the memory driver and start the main loop. */
  struct sigaction sa;

  sa.sa_handler = SIG_MESS;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if (sigaction(SIGTERM,&sa,NULL)<0) panic("MEM","sigaction failed", errno);

  m_init();			
  driver_task(&m_dtab);		
  return(OK);				
}

/*===========================================================================*
 *				 m_name					     *
 *===========================================================================*/
PRIVATE char *m_name()
{
/* Return a name for the current device. */
  static char name[] = "memory";
  return name;  
}

/*===========================================================================*
 *				m_prepare				     *
 *===========================================================================*/
PRIVATE struct device *m_prepare(device)
int device;
{
/* Prepare for I/O on a device: check if the minor device number is ok. */
  if (device < 0 || device >= NR_DEVS) return(NIL_DEV);
  m_device = device;

  return(&m_geom[device]);
}

/*===========================================================================*
 *				m_transfer				     *
 *===========================================================================*/
PRIVATE int m_transfer(proc_nr, opcode, pos64, iov, nr_req, safe)
int proc_nr;			/* process doing the request */
int opcode;			/* DEV_GATHER or DEV_SCATTER */
u64_t pos64;			/* offset on device to read or write */
iovec_t *iov;			/* pointer to read or write request vector */
unsigned nr_req;		/* length of request vector */
int safe;			/* safe copies */
{
/* Read or write one the driver's minor devices. */
  phys_bytes mem_phys;
  int seg;
  unsigned count, left, chunk;
  vir_bytes user_vir, vir_offset = 0;
  phys_bytes user_phys;
  struct device *dv;
  unsigned long dv_size;
  int s, r;
  off_t position;

  static int n = 0;

  if (ex64hi(pos64) != 0)
	return OK;	/* Beyond EOF */
  position= cv64ul(pos64);

  /* Get minor device number and check for /dev/null. */
  dv = &m_geom[m_device];
  dv_size = cv64ul(dv->dv_size);

  while (nr_req > 0) {

	/* How much to transfer and where to / from. */
	count = iov->iov_size;
	user_vir = iov->iov_addr;

	switch (m_device) {

	/* No copying; ignore request. */
	case NULL_DEV:
	    if (opcode == DEV_GATHER) return(OK);	/* always at EOF */
	    break;

	/* Virtual copying. For RAM disk, kernel memory and boot device. */
	case RAM_DEV:
	case KMEM_DEV:
	case BOOT_DEV:
	    if (position >= dv_size) return(OK); 	/* check for EOF */
	    if (position + count > dv_size) count = dv_size - position;
	    seg = m_seg[m_device];

	    if (opcode == DEV_GATHER) {			/* copy actual data */
	      if(safe) {
	        r=sys_safecopyto(proc_nr, user_vir, vir_offset,
	  	  position, count, seg);
	      } else {
	        r=sys_vircopy(SELF,seg,position, 
			proc_nr,D,user_vir+vir_offset, count);
	      }
	    } else {
	      if(safe) {
	        r=sys_safecopyfrom(proc_nr, user_vir, vir_offset,
	  	  position, count, seg);
	      } else {
	        r=sys_vircopy(proc_nr,D,user_vir+vir_offset,
			SELF,seg,position, count);
	      }
	    }
	    if(r != OK) {
              panic("MEM","I/O copy failed",r);
	    }
	    break;

	/* Physical copying. Only used to access entire memory. */
	case MEM_DEV:
	    if (position >= dv_size) return(OK); 	/* check for EOF */
	    if (position + count > dv_size) count = dv_size - position;
	    mem_phys = cv64ul(dv->dv_base) + position;
	    if((r=sys_umap(proc_nr, safe ? GRANT_SEG : D, user_vir,
		count + vir_offset, &user_phys)) != OK) {
                    panic("MEM","sys_umap failed in m_transfer",r);
	    }

	    if (opcode == DEV_GATHER) {			/* copy data */
	        sys_physcopy(NONE, PHYS_SEG, mem_phys, 
	        	NONE, PHYS_SEG, user_phys + vir_offset, count);
	    } else {
	        sys_physcopy(NONE, PHYS_SEG, user_phys + vir_offset, 
	        	NONE, PHYS_SEG, mem_phys, count);
	    }
	    break;

	/* Null byte stream generator. */
	case ZERO_DEV:
	    if (opcode == DEV_GATHER) {
		size_t suboffset = 0;
	        left = count;
	    	while (left > 0) {
	    	    chunk = (left > ZERO_BUF_SIZE) ? ZERO_BUF_SIZE : left;
		    if(safe) {
	             s=sys_safecopyto(proc_nr, user_vir,
		       vir_offset+suboffset, (vir_bytes) dev_zero, chunk, D);
		    } else {
	    	      s=sys_vircopy(SELF, D, (vir_bytes) dev_zero, 
	    	            proc_nr, D, user_vir + vir_offset+suboffset,
			    chunk);
		    }
		    if(s != OK)
	    	        report("MEM","sys_vircopy failed", s);
	    	    left -= chunk;
 	            suboffset += chunk;
	    	}
	    }
	    break;

	case IMGRD_DEV:
	    if (position >= dv_size) return(OK); 	/* check for EOF */
	    if (position + count > dv_size) count = dv_size - position;

	    if (opcode == DEV_GATHER) {			/* copy actual data */
	      if(safe) {
	          s=sys_safecopyto(proc_nr, user_vir, vir_offset,
	  	     (vir_bytes)&imgrd[position], count, D);
	      } else {
	        s=sys_vircopy(SELF, D, (vir_bytes)&imgrd[position],
			proc_nr, D, user_vir+vir_offset, count);
	      }
	    } else {
	      if(safe) {
	          s=sys_safecopyfrom(proc_nr, user_vir, vir_offset,
	  	     (vir_bytes)&imgrd[position], count, D);
	      } else {
	          s=sys_vircopy(proc_nr, D, user_vir+vir_offset,
			SELF, D, (vir_bytes)&imgrd[position], count);
	      }
	    }
	    break;

	/* Unknown (illegal) minor device. */
	default:
	    return(EINVAL);
	}

	/* Book the number of bytes transferred. */
	position += count;
	vir_offset += count;
  	if ((iov->iov_size -= count) == 0) { iov++; nr_req--; vir_offset = 0; }

  }
  return(OK);
}

/*===========================================================================*
 *				m_do_open				     *
 *===========================================================================*/
PRIVATE int m_do_open(dp, m_ptr)
struct driver *dp;
message *m_ptr;
{
  int r;

/* Check device number on open. */
  if (m_prepare(m_ptr->DEVICE) == NIL_DEV) return(ENXIO);
  if (m_device == MEM_DEV)
  {
	r = sys_enable_iop(m_ptr->IO_ENDPT);
	if (r != OK)
	{
		printf("m_do_open: sys_enable_iop failed for %d: %d\n",
			m_ptr->IO_ENDPT, r);
		return r;
	}
  }
  return(OK);
}

/*===========================================================================*
 *				m_init					     *
 *===========================================================================*/
PRIVATE void m_init()
{
  /* Initialize this task. All minor devices are initialized one by one. */
  u32_t ramdev_size;
  u32_t ramdev_base;
  message m;
  int i, s;

  if (OK != (s=sys_getkinfo(&kinfo))) {
      panic("MEM","Couldn't get kernel information.",s);
  }

  /* Install remote segment for /dev/kmem memory. */
  m_geom[KMEM_DEV].dv_base = cvul64(kinfo.kmem_base);
  m_geom[KMEM_DEV].dv_size = cvul64(kinfo.kmem_size);
  if (OK != (s=sys_segctl(&m_seg[KMEM_DEV], (u16_t *) &s, (vir_bytes *) &s, 
  		kinfo.kmem_base, kinfo.kmem_size))) {
      panic("MEM","Couldn't install remote segment.",s);
  }

  /* Install remote segment for /dev/boot memory, if enabled. */
  m_geom[BOOT_DEV].dv_base = cvul64(kinfo.bootdev_base);
  m_geom[BOOT_DEV].dv_size = cvul64(kinfo.bootdev_size);
  if (kinfo.bootdev_base > 0) {
      if (OK != (s=sys_segctl(&m_seg[BOOT_DEV], (u16_t *) &s, (vir_bytes *) &s, 
              kinfo.bootdev_base, kinfo.bootdev_size))) {
          panic("MEM","Couldn't install remote segment.",s);
      }
  }

  /* See if there are already RAM disk details at the Data Store server. */
  if(ds_retrieve_u32(MY_DS_NAME_BASE, &ramdev_base) == OK &&
     ds_retrieve_u32(MY_DS_NAME_SIZE, &ramdev_size) == OK) {
  	printf("MEM retrieved size %u and base %u from DS, status %d\n",
    		ramdev_size, ramdev_base, s);
  	if (OK != (s=sys_segctl(&m_seg[RAM_DEV], (u16_t *) &s, 
		(vir_bytes *) &s, ramdev_base, ramdev_size))) {
      		panic("MEM","Couldn't install remote segment.",s);
  	}
  	m_geom[RAM_DEV].dv_base = cvul64(ramdev_base);
 	m_geom[RAM_DEV].dv_size = cvul64(ramdev_size);
	printf("MEM stored retrieved details as new RAM disk\n");
  }

  /* Ramdisk image built into the memory driver */
  m_geom[IMGRD_DEV].dv_base= cvul64(0);
  m_geom[IMGRD_DEV].dv_size= cvul64(imgrd_size);

  /* Initialize /dev/zero. Simply write zeros into the buffer. */
  for (i=0; i<ZERO_BUF_SIZE; i++) {
       dev_zero[i] = '\0';
  }

  /* Set up memory ranges for /dev/mem. */
#if (CHIP == INTEL)
  if (OK != (s=sys_getmachine(&machine))) {
      panic("MEM","Couldn't get machine information.",s);
  }
  if (! machine.prot) {
	m_geom[MEM_DEV].dv_size =   cvul64(0x100000); /* 1M for 8086 systems */
  } else {
#if _WORD_SIZE == 2
	m_geom[MEM_DEV].dv_size =  cvul64(0x1000000); /* 16M for 286 systems */
#else
	m_geom[MEM_DEV].dv_size = cvul64(0xFFFFFFFF); /* 4G-1 for 386 systems */
#endif
  }
#else /* !(CHIP == INTEL) */
#if (CHIP == M68000)
  m_geom[MEM_DEV].dv_size = cvul64(MEM_BYTES);
#else /* !(CHIP == M68000) */
#error /* memory limit not set up */
#endif /* !(CHIP == M68000) */
#endif /* !(CHIP == INTEL) */
}

/*===========================================================================*
 *				m_ioctl					     *
 *===========================================================================*/
PRIVATE int m_ioctl(dp, m_ptr, safe)
struct driver *dp;			/* pointer to driver structure */
message *m_ptr;				/* pointer to control message */
int safe;
{
/* I/O controls for the memory driver. Currently there is one I/O control:
 * - MIOCRAMSIZE: to set the size of the RAM disk.
 */
  struct device *dv;

  switch (m_ptr->REQUEST) {
    case MIOCRAMSIZE: {
	/* Someone wants to create a new RAM disk with the given size. */
	static int first_time= 1;

	u32_t ramdev_size;
	phys_bytes ramdev_base;
	message m;
	int s;

	/* A ramdisk can be created only once, and only on RAM disk device. */
	if (!first_time) return(EPERM);
	if (m_ptr->DEVICE != RAM_DEV) return(EINVAL);
        if ((dv = m_prepare(m_ptr->DEVICE)) == NIL_DEV) return(ENXIO);

#if 0
	ramdev_size= m_ptr->POSITION;
#else
	/* Get request structure */
	if(safe) {
	   s= sys_safecopyfrom(m_ptr->IO_ENDPT, (vir_bytes)m_ptr->IO_GRANT,
		0, (vir_bytes)&ramdev_size, sizeof(ramdev_size), D);
	} else {
	   s= sys_vircopy(m_ptr->IO_ENDPT, D, (vir_bytes)m_ptr->ADDRESS,
		SELF, D, (vir_bytes)&ramdev_size, sizeof(ramdev_size));
	}

	if (s != OK)
		return s;
#endif

#if DEBUG
	printf("allocating ramdisk of size 0x%x\n", ramdev_size);
#endif

	/* Try to allocate a piece of memory for the RAM disk. */
        if (allocmem(ramdev_size, &ramdev_base) < 0) {
            report("MEM", "warning, allocmem failed", errno);
            return(ENOMEM);
        }

	/* Store the values we got in the data store so we can retrieve
	 * them later on, in the unfortunate event of a crash.
	 */
	if(ds_publish_u32(MY_DS_NAME_BASE, ramdev_base) != OK ||
	   ds_publish_u32(MY_DS_NAME_SIZE, ramdev_size) != OK) {
      		panic("MEM","Couldn't store RAM disk details at DS.",s);
	}

#if DEBUG
	printf("MEM stored size %u and base %u at DS, names %s and %s\n",
	    ramdev_size, ramdev_base, MY_DS_NAME_BASE, MY_DS_NAME_SIZE);
#endif

  	if (OK != (s=sys_segctl(&m_seg[RAM_DEV], (u16_t *) &s, 
		(vir_bytes *) &s, ramdev_base, ramdev_size))) {
      		panic("MEM","Couldn't install remote segment.",s);
  	}

	dv->dv_base = cvul64(ramdev_base);
	dv->dv_size = cvul64(ramdev_size);
	/* first_time= 0; */
	break;
    }
    case MIOCMAP:
    case MIOCUNMAP: {
    	int r, do_map;
    	struct mapreq mapreq;

	if ((*dp->dr_prepare)(m_ptr->DEVICE) == NIL_DEV) return(ENXIO);
    	if (m_device != MEM_DEV)
    		return ENOTTY;

	do_map= (m_ptr->REQUEST == MIOCMAP);	/* else unmap */

	/* Get request structure */
	if(safe) {
	   r= sys_safecopyfrom(m_ptr->IO_ENDPT, (vir_bytes)m_ptr->IO_GRANT,
		0, (vir_bytes)&mapreq, sizeof(mapreq), D);
	} else {
	   r= sys_vircopy(m_ptr->IO_ENDPT, D, (vir_bytes)m_ptr->ADDRESS,
		SELF, D, (vir_bytes)&mapreq, sizeof(mapreq));
	}

	if (r != OK)
		return r;
	r= sys_vm_map(m_ptr->IO_ENDPT, do_map,
		(phys_bytes)mapreq.base, mapreq.size, mapreq.offset);
	return r;
    }

    default:
  	return(do_diocntl(&m_dtab, m_ptr, safe));
  }
  return(OK);
}

/*===========================================================================*
 *				m_geometry				     *
 *===========================================================================*/
PRIVATE void m_geometry(entry)
struct partition *entry;
{
  /* Memory devices don't have a geometry, but the outside world insists. */
  entry->cylinders = div64u(m_geom[m_device].dv_size, SECTOR_SIZE) / (64 * 32);
  entry->heads = 64;
  entry->sectors = 32;
}

