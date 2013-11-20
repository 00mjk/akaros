/* 
 * sdsci.c
 * from Inferno
 */
#include <vfs.h>
#include <kfs.h>
#include <slab.h>
#include <kmalloc.h>
#include <kref.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <error.h>
#include <cpio.h>
#include <pmap.h>
#include <smp.h>
#include <sd.h>

static int
scsitest(struct sdreq* r)
{
	r->write = 0;
	memset(r->cmd, 0, sizeof(r->cmd));
	r->cmd[1] = r->lun<<5;
	r->clen = 6;
	r->data = NULL;
	r->dlen = 0;
	r->flags = 0;

	r->status = ~0;

	return r->unit->dev->ifc->rio(r);
}

int
scsiverify(struct sdunit* unit)
{
	struct sdreq *r;
	int i, status;
	uint8_t *inquiry;

	if((r = kzmalloc(sizeof(struct sdreq), 0)) == NULL)
		return 0;
	if((inquiry = sdmalloc(sizeof(unit->inquiry))) == NULL){
		kfree(r);
		return 0;
	}
	r->unit = unit;
	r->lun = 0;		/* ??? */

	memset(unit->inquiry, 0, sizeof(unit->inquiry));
	r->write = 0;
	r->cmd[0] = 0x12;
	r->cmd[1] = r->lun<<5;
	r->cmd[4] = sizeof(unit->inquiry)-1;
	r->clen = 6;
	r->data = inquiry;
	r->dlen = sizeof(unit->inquiry)-1;
	r->flags = 0;

	r->status = ~0;
	if(unit->dev->ifc->rio(r) != SDok){
		kfree(r);
		return 0;
	}
	memmove(unit->inquiry, inquiry, r->dlen);
	kfree(inquiry); 

	for(i = 0; i < 3; i++){
		while((status = scsitest(r)) == SDbusy)
			;
		if(status == SDok || status != SDcheck)
			break;
		if(!(r->flags & SDvalidsense))
			break;
		if((r->sense[2] & 0x0F) != 0x02)
			continue;

		/*
		 * Unit is 'not ready'.
		 * If it is in the process of becoming ready or needs
		 * an initialising command, set status so it will be spun-up
		 * below.
		 * If there's no medium, that's OK too, but don't
		 * try to spin it up.
		 */
		if(r->sense[12] == 0x04){
			if(r->sense[13] == 0x02 || r->sense[13] == 0x01){
				status = SDok;
				break;
			}
		}
		if(r->sense[12] == 0x3A)
			break;
	}

	if(status == SDok){
		/*
		 * Try to ensure a direct-access device is spinning.
		 * Don't wait for completion, ignore the result.
		 */
		if((unit->inquiry[0] & 0x1F) == 0){
			memset(r->cmd, 0, sizeof(r->cmd));
			r->write = 0;
			r->cmd[0] = 0x1B;
			r->cmd[1] = (r->lun<<5)|0x01;
			r->cmd[4] = 1;
			r->clen = 6;
			r->data = NULL;
			r->dlen = 0;
			r->flags = 0;

			r->status = ~0;
			unit->dev->ifc->rio(r);
		}
	}
	kfree(r);

	if(status == SDok || status == SDcheck)
		return 1;
	return 0;
}

static int
scsirio(struct sdreq* r)
{
	ERRSTACK(1);
	/*
	 * Perform an I/O request, returning
	 *	-1	failure
	 *	 0	ok
	 *	 1	no medium present
	 *	 2	retry
	 * The contents of r may be altered so the
	 * caller should re-initialise if necesary.
	 */
	r->status = ~0;
	switch(r->unit->dev->ifc->rio(r)){
	default:
		return -1;
	case SDcheck:
		if(!(r->flags & SDvalidsense))
			return -1;
		switch(r->sense[2] & 0x0F){
		case 0x00:		/* no sense */
		case 0x01:		/* recovered error */
			return 2;
		case 0x06:		/* check condition */
			/*
			 * 0x28 - not ready to ready transition,
			 *	  medium may have changed.
			 * 0x29 - power on or some type of reset.
			 */
			if(r->sense[12] == 0x28 && r->sense[13] == 0)
				return 2;
			if(r->sense[12] == 0x29)
				return 2;
			return -1;
		case 0x02:		/* not ready */
			/*
			 * If no medium present, bail out.
			 * If unit is becoming ready, rather than not
			 * not ready, wait a little then poke it again. 				 */
			if(r->sense[12] == 0x3A)
				return 1;
			if(r->sense[12] != 0x04 || r->sense[13] != 0x01)
				return -1;

			while(waserror())
				;
#warning "how do we do this sleep on up->sleep?"		       
			//rendez_sleep_timeout(&up->sleep, return0, 0, 500);
			poperror();
			scsitest(r);
			return 2;
		default:
			return -1;
		}
		return -1;
	case SDok:
		return 0;
	}
	return -1;
}

int
scsionline(struct sdunit* unit)
{
	struct sdreq *r;
	uint8_t *p;
	int ok, retries;

	if((r = kzmalloc(sizeof(struct sdreq), 0)) == NULL)
		return 0;
	if((p = sdmalloc(8)) == NULL){
		kfree(r);
		return 0;
	}

	ok = 0;

	r->unit = unit;
	r->lun = 0;				/* ??? */
	for(retries = 0; retries < 10; retries++){
		/*
		 * Read-capacity is mandatory for DA, WORM, CD-ROM and
		 * MO. It may return 'not ready' if type DA is not
		 * spun up, type MO or type CD-ROM are not loaded or just
		 * plain slow getting their act together after a reset.
		 */
		r->write = 0;
		memset(r->cmd, 0, sizeof(r->cmd));
		r->cmd[0] = 0x25;
		r->cmd[1] = r->lun<<5;
		r->clen = 10;
		r->data = p;
		r->dlen = 8;
		r->flags = 0;
	
		r->status = ~0;
		switch(scsirio(r)){
		default:
			break;
		case 0:
			unit->sectors = (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3];
			if(unit->sectors == 0)
				continue;
			/*
			 * Read-capacity returns the LBA of the last sector,
			 * therefore the number of sectors must be incremented.
			 */
			unit->sectors++;
			unit->secsize = (p[4]<<24)|(p[5]<<16)|(p[6]<<8)|p[7];

			/*
			 * Some ATAPI CD readers lie about the block size.
			 * Since we don't read audio via this interface
			 * it's okay to always fudge this.
			 */
			if(unit->secsize == 2352)
				unit->secsize = 2048;
			ok = 1;
			break;
		case 1:
			ok = 1;
			break;
		case 2:
			continue;
		}
		break;
	}
	kfree(p);
	kfree(r);

	if(ok)
		return ok+retries;
	else
		return 0;
}

int
scsiexec(struct sdunit* unit, int write,
	 uint8_t* cmd, int clen, void* data, int* dlen)
{
	struct sdreq *r;
	int status;

	if((r = kzmalloc(sizeof(struct sdreq), 0)) == NULL)
		return SDmalloc;
	r->unit = unit;
	r->lun = cmd[1]>>5;		/* ??? */
	r->write = write;
	memmove(r->cmd, cmd, clen);
	r->clen = clen;
	r->data = data;
	if(dlen)
		r->dlen = *dlen;
	r->flags = 0;

	r->status = ~0;

	/*
	 * Call the device-specific I/O routine.
	 * There should be no calls to 'error()' below this
	 * which percolate back up.
	 */
	switch(status = unit->dev->ifc->rio(r)){
	case SDok:
		if(dlen)
			*dlen = r->rlen;
		/*FALLTHROUGH*/
	case SDcheck:
		/*FALLTHROUGH*/
	default:
		/*
		 * It's more complicated than this. There are conditions
		 * which are 'ok' but for which the returned status code
		 * is not 'SDok'.
		 * Also, not all conditions require a reqsense, might
		 * need to do a reqsense here and make it available to the
		 * caller somehow.
		 *
		 * Mañana.
		 */
		break;
	}
	sdfree(r);

	return status;
}

long
scsibio(struct sdunit* unit, int lun, int write, void* data, long nb, long bno)
{
	struct sdreq *r;
	long rlen;

	if((r = kzmalloc(sizeof(struct sdreq), 0)) == NULL)
		error(Enomem);
	r->unit = unit;
	r->lun = lun;
again:
	r->write = write;
	if(write == 0)
		r->cmd[0] = 0x28;
	else
		r->cmd[0] = 0x2A;
	r->cmd[1] = (lun<<5);
	r->cmd[2] = bno>>24;
	r->cmd[3] = bno>>16;
	r->cmd[4] = bno>>8;
	r->cmd[5] = bno;
	r->cmd[6] = 0;
	r->cmd[7] = nb>>8;
	r->cmd[8] = nb;
	r->cmd[9] = 0;
	r->clen = 10;
	r->data = data;
	r->dlen = nb*unit->secsize;
	r->flags = 0;

	r->status = ~0;
	switch(scsirio(r)){
	default:
		rlen = -1;
		break;
	case 0:
		rlen = r->rlen;
		break;
	case 2:
		rlen = -1;
		if(!(r->flags & SDvalidsense))
			break;
		switch(r->sense[2] & 0x0F){
		default:
			break;
		case 0x06:		/* check condition */
			/*
			 * Check for a removeable media change.
			 * If so, mark it by zapping the geometry info
			 * to force an online request.
			 */
			if(r->sense[12] != 0x28 || r->sense[13] != 0)
				break;
			if(unit->inquiry[1] & 0x80)
				unit->sectors = 0;
			break;
		case 0x02:		/* not ready */
			/*
			 * If unit is becoming ready,
			 * rather than not not ready, try again.
			 */
			if(r->sense[12] == 0x04 && r->sense[13] == 0x01)
				goto again;
			break;
		}
		break;
	}
	kfree(r);

	return rlen;
}

struct sdev*
scsiid(struct sdev* sdev, struct sdifc* ifc)
{
	char name[32];
	static char idno[16] = "0123456789abcdef";
	static char *p = idno;

	while(sdev){
		if(sdev->ifc == ifc){
			sdev->idno = *p++;
			snprintf(name, sizeof(name), "sd%c", sdev->idno);
			kstrdup(&sdev->name, name);
			if(p >= &idno[sizeof(idno)])
				break;
		}
		sdev = sdev->next;
	}

	return NULL;
}
