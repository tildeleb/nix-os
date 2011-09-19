/*
 * A single session is multiplexed among channels
 *
 * Clients create channels, send messages through channels,
 * and receive new channels and gone channels.
 * Messages must be received from returned channels by clients.
 * (NB: where channel has type Ch*)
 *
 *
 * Termination:
 *	chwproc is a client for a tses (writer)
 *	cmuxproc is a client for a tses (reader)
 *
 *	- If the session gets and eof or error during read
 *	1. nil is sent to cmux
 *	2. it informs the client (for both cmux and all chans)
 *	3. the client closes the chans and the mux
 *	4. cmux sends nil to tses wc.
 *	- If the client terminates the session
 *	1. nil is sent to chwproc
 *	3. it informs cmux, which will also get nil from tses
 *	   and playes the tses close protocol.
 *
 *	Sending a message with last releases the chan for writing.
 *	receving a message with last releases the chan for reading.
 *	The message may have no data, in which case it's used just to
 *	notify that the channel is being released.
 *	A channel is gone only when it's released both for reading and
 *	writing.
 *	Normal termination is:
 *		send last and recv last
 *		recv last and send last
 *	flushing:
 *		abortch
 *	early close:
 *		closech
 *		drainch
 *	eof:
 *		send last (null msg ok)
 */
#include <u.h>
#include <libc.h>
#include <thread.h>
#include <error.h>
#include <fcall.h>	/* *BIG* macros */
#include "conf.h"
#include "msg.h"
#include "ch.h"
#include "dbg.h"

typedef struct Chmsg Chmsg;

struct Chmsg
{
	Msg *m;
	int last;
};

static void
cmdump(Cmux *cm)
{
	int i;

	if(dbg['s'] == 0)
		return;
	print("cm %p: %d chs:", cm, cm->nchs);
	for(i = 0; i < cm->nchs; i++)
		if(cm->chs[i] != nil){
			if(cm->chs[i]->dead == 0)
				print(" %d", i);
			else
				print(" %dd", i);
			assert(cm->chs[i]->id == i);
		}
	print("\n");
}

extern void dumpmsg(Msg*);

/*
 * mux messages sent to ch->wc into cm->swc
 * terminates when a nil is received.
 */
static void
chwproc(void *a)
{
	Ch *ch;
	int nmsgs;
	uint flags;
	uchar *buf;
	Chmsg chm;
	Cmux *cm;
	Msg *m;

	ch  = a;
	threadsetname("chwproc %p", a);
	dpprint("chwproc[%p]: started\n", a);
Resurrection:
	assert(ch->id <= CFidmask);
	cm = ch->cm;
	for(nmsgs = 0; recv(ch->wc, &chm) == 1; nmsgs++){
		m = chm.m;
		if(m == nil){
			dsprint("chwproc[%p]: ch%d: nil msg\n", a, ch->id);
			break;
		}
		flags = 0;
		if(chm.last)
			flags |= CFend;
		if(nmsgs == 0 && ch->notnew == 0)
			flags |= CFnew;
		buf = msgpushhdr(m, BIT16SZ);
		PBIT16(buf, flags|ch->id);
		if(dbg['s']>1)
			print("chwproc[%p]: hdr %#x\n", a, flags|ch->id);
		if(sendp(cm->swc, m) != 1){
			dprint("chwproc[%p]: send failed", a);
			freemsg(m);
			break;
		}
		if(chm.last != 0){
			while(nbrecv(ch->wc, &chm) == 1){
				dprint("chwproc[%p]: extra msg\n", a);
				freemsg(chm.m);
			}
			break;
		}
	}
	sendp(cm->endc, ch);
	dpprint("chwproc[%p]: limbo\n", a);
	if(recvul(ch->dc) != 0){
		dpprint("chwproc[%p]: resurrection\n", a);
		goto Resurrection;
	}
	dpprint("chwproc[%p]: exiting\n", a);
	chanfree(ch->dc);
}


/*
 * A channel might have been terminated for writing, yet the
 * peer may be still busy processing outgoing requests.
 * If the channel is aborted, we send hup request to the server
 * to let it know that we will drop any further reply to this channel.
 */
static void
wendch(Ch *ch)
{
	Msg *m;
	uchar *buf;

	m = emalloc(sizeof *m);
	buf = msgpushhdr(m, BIT16SZ);
	PBIT16(buf, CFend|ch->id);
	sendp(ch->cm->swc, m);
}

static Ch*
mkch(Cmux *cm, int i, int notnew)
{
	Ch *ch;

	ch = cm->chs[i];
	if(ch != nil){
		assert(ch->dead != 0);
		ch->notnew = notnew;
		ch->rclosed = ch->wclosed = ch->flushing = 0;
		sendul(ch->dc, 1);	/* resurrect chwproc */
		ch->dead = 0;
		return ch;
	}
	ch = emalloc(sizeof *ch);
	cm->chs[i] = ch;
	ch->id = i;
	ch->rc = echancreate(sizeof(Chmsg), 0);
	ch->wc = echancreate(sizeof(Chmsg), 0);
	ch->dc = echancreate(sizeof(ulong), 0);
	ch->cm = cm;
	ch->notnew = notnew;
	threadcreate(chwproc, ch, Stack);
	return ch;
}

static void
grow(Cmux *cm, int n)
{
	int elsz;

	if(n > CFidmask)
		sysfatal("growto: no more channel ids");
	if(cm->nachs < n){
		elsz = sizeof cm->chs[0];
		cm->chs = erealloc(cm->chs, n * elsz);
		memset(&cm->chs[cm->nachs], 0, (n - cm->nachs) * elsz);
		cm->nachs = n;
	}
}

static Ch*
addch(Cmux *cm, int i, int notnew)
{
	if(i < 0)
		for(i =0 ; i < cm->nchs; i++)
			if(cm->chs[i] == nil || cm->chs[i]->dead)
				break;
	if(i >= cm->nchs){
		grow(cm, i+1);
		cm->nchs = i+1;
	}
	if(cm->chs[i] != nil && cm->chs[i]->dead == 0){
		werrstr("channel %d in use", i);
		return nil;
	}
	cm->nuse++;
	dsprint("cmuxproc[%p]: addch: ch%d (%d chs)\n", cm, i, cm->nuse);
	return mkch(cm, i, notnew);
}

enum{Free = 0, Keep = 1};

static void
delch(Cmux *cm, Ch *ch, int keep)
{
	
	dsprint("cmux[%p]: delch: ch%d keep=%d (%d chs)\n", cm, ch->id, keep, cm->nuse);
	if(ch->dead == 0 || keep != 0)
		cm->nuse--;
	if(keep)
		ch->dead = 1;
	else{
		chanfree(ch->rc);
		chanfree(ch->wc);
		sendul(ch->dc, 0);
		ch->rc = ch->wc = nil;
		assert(cm->chs[ch->id] == ch);
		cm->chs[ch->id] = nil;
		free(ch);
	}
}

/*
 * Demux message already received in cm->rc and return
 * the Ch for it, perhaps a new Ch.
 */
static Ch*
cdemux(Cmux *cm, Msg *m, int *lastp)
{
	uint id, flags;
	uchar *hdr;

	Ch *ch;

	if(msglen(m) < BIT16SZ){
		werrstr("short message header");
		return nil;
	}
	hdr = msgpophdr(m, BIT16SZ);
	assert(hdr != nil);
	flags = GBIT16(hdr);
	if(dbg['s']>1)
		print("cdemux: hdr %#x\n", flags);
	id = flags & CFidmask;
	if(flags&CFnew){
		ch = addch(cm, id, 1);
		if(ch == nil)
			return nil;
		sendp(cm->newc, ch);
	}else
		if(id >= cm->nchs || cm->chs[id] == nil || cm->chs[id]->dead){
			werrstr("no channel %d", id);
			return nil;
		}
	*lastp = flags&CFend;
	return cm->chs[id];
}

static void
allclosed(Cmux *cm)
{
	int i;

	for(i = 0; i < cm->nchs; i++)
		if(cm->chs[i] != nil)
			if(cm->chs[i]->dead)
				delch(cm, cm->chs[i], Free);
			else{
				fprint(2,"abort: cmuxproc: terminated but ch%d open", i);
				abort();
			}
}

static void
allabort(Cmux *cm)
{
	int i;
	Chmsg chm;

	for(i = 0; i < cm->nchs; i++)
		if(cm->chs[i] != nil)
			if(cm->chs[i]->dead == 0){
				if(!cm->chs[i]->rclosed){
					chm.m = nil;
					chm.last = 1;
					send(cm->chs[i]->rc, &chm);
					cm->chs[i]->rclosed = 1;
				}
				if(cm->chs[i]->wclosed)
					delch(cm, cm->chs[i], Free);
			}else
				delch(cm, cm->chs[i], Free);
}

static void
cmuxproc(void *a)
{
	enum{Mkc, Endc, Rc};
	Cmux *cm;
	ulong dummy;
	char *err;
	Chmsg chm;
	Msg *m;
	Ch *ch;
	Alt alts[] = {
		{nil, &dummy, CHANRCV},
		{nil, &ch, CHANRCV},
		{nil, &m, CHANRCV},
		{nil, nil, CHANEND},
	};

	cm = a;
	threadsetname("cmuxproc %p", a);
	dpprint("cmuxproc[%p]: starting\n", a);
	alts[0].c = cm->mkc;
	alts[1].c = cm->endc;
	alts[2].c = cm->src;
	for(;;)
		switch(alt(alts)){
		case Mkc:
			ch = addch(cm, -1, 0);
			sendp(cm->mkrc, ch);
			break;
		case Endc:
			if(ch == nil){
				allclosed(cm);
				sendp(cm->swc, nil);
				while((m = recvp(cm->src)) != nil)
					freemsg(m);
				free(recvp(cm->sec));
				goto Done;
			}
			if(ch->wclosed){
				dsprint("cmuxproc[%p]: ch%d flushing\n", a, ch->id);
				ch->flushing = 1;
				wendch(ch);
			}else
				ch->wclosed = 1;
			if(ch->rclosed)
				delch(cm, ch, Keep);
			break;
		case Rc:
			if(m == nil){
				err = recvp(cm->sec);
				if(err == nil)
					dsprint("cmuxproc[%p]: eof (%d chs)\n", a, cm->nuse);
				else
					dsprint("cmuxproc[%p]: %s\n", a, err);
				free(err);
				sendp(cm->newc, nil);
				if(cm->nuse > 0){
					allabort(cm);
					break;
				}
				sendp(cm->swc, nil);
				recvp(cm->endc);
				goto Done;
			}
			chm.m = m;
			ch = cdemux(cm, m, &chm.last);
			if(ch == nil){
				dsprint("cdemux: drop msg: %r\n");
				cmdump(cm);
				freemsg(m);
				break;
			}
			if(!ch->rclosed && !ch->flushing)
				send(ch->rc, &chm);
			else
				freemsg(m);
			if(chm.last){
				if(ch->rclosed){
					dsprint("cmuxproc[%p]: ch%d flush\n", a, ch->id);
					ch->flushing = 1;
				}
				ch->rclosed = 1;
				if(ch->wclosed)
					delch(cm, ch, Keep);
			}
			break;
		default:
			sysfatal("cmuxproc[%p]: alt", a);
		}
Done:
	allclosed(cm);
	dsprint("cmuxproc[%p]: done\n", a);
	chanfree(cm->newc);
	chanfree(cm->mkc);
	chanfree(cm->mkrc);
	chanfree(cm->endc);
	free(cm->chs);
	free(cm);
	threadexits(nil);
}

Cmux*
muxses(Channel *src, Channel *swc, Channel *sec)
{
	Cmux *cm;

	cm = emalloc(sizeof *cm);
	cm->src = src;
	cm->swc = swc;
	cm->sec = sec;
	cm->newc = echancreate(sizeof(Ch*), 0);
	cm->mkc = echancreate(sizeof(ulong), 0);
	cm->mkrc = echancreate(sizeof(Ch*), 0);
	cm->endc = echancreate(sizeof(ulong), 0);
	/*
	 * Could be a thread, but then the caller
	 * would have to be aware and don't block the process.
	 */
	proccreate(cmuxproc, cm, Stack);
	return cm;
}

void
closemux(Cmux *cm)
{
	dsprint("closemux %p\n", cm);
	sendp(cm->endc, nil);
}

Ch*
newch(Cmux *cm)
{
	sendul(cm->mkc, 0);
	return recvp(cm->mkrc);
}

int
chsend(Ch *ch, Msg *m, int last)
{
	Chmsg chm;

	assert(m != nil);
	chm.m = m;
	chm.last = last;
	if(ch->flushing){
		freemsg(m);
		chm.m = nil;
		chm.last = 1;
		werrstr("chsend: ch%d flushing", ch->id);
		send(ch->wc, &chm);

		/*
		 * client is sending which means that the last
		 * message was not sent.
		 * We are flushing and asking the wproc to exit,
		 * so we must notify the peer that we are done.
		 */
		wendch(ch);

		return -1;
	}
	send(ch->wc, &chm);
	return 0;
}

Msg*
chrecv(Ch *ch, int *last)
{
	Chmsg chm;

	recv(ch->rc, &chm);
	*last = chm.last;
	return chm.m;
}

/*
 * Close a Ch without sending a msg with the last request.
 * The caller is expected to read until learning that the peer is
 * done, unless it knows the peer is already done.
 */
void
closech(Ch *ch)
{
	Chmsg chm;

	dsprint("closech: ch%d\n", ch->id);
	chm.m = nil;
	chm.last = 1;
	send(ch->wc, &chm);
	wendch(ch);
}

/*
 * Close a Ch after having sent the last msg through it.
 * Note that cmuxproc might be trying to send a reply to our caller.
 */
void
abortch(Ch *ch)
{
	Chmsg chm;

	Alt alts[] = {
		{ch->cm->endc, &ch, CHANSND},
		{ch->rc, &chm, CHANRCV},
		{nil, nil, CHANEND}
	};

	dsprint("abortch: ch%d\n", ch->id);
	for(;;){
		switch(alt(alts)){
		case 0:
			/* could notify our termination request */
			return;
		case 1:
			freemsg(chm.m);
			if(chm.last){
				/* got the last msg; ch is gone */
				return;
			}
			break;
		default:
			sysfatal("alt");
		}
	}
}

void
drainch(Ch *ch)
{
	Msg *m;
	int last;

	dsprint("drainch: ch%d\n", ch->id);
	while((m = chrecv(ch, &last)) != nil){
		freemsg(m);
		if(last)
			break;
	}	
}
