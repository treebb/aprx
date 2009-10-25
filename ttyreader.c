/* **************************************************************** *
 *                                                                  *
 *  APRX -- 2nd generation receive-only APRS-i-gate with            *
 *          minimal requirement of esoteric facilities or           *
 *          libraries of any kind beyond UNIX system libc.          *
 *                                                                  *
 * (c) Matti Aarnio - OH2MQK,  2007-2009                            *
 *                                                                  *
 * **************************************************************** */

#define _SVID_SOURCE 1

#include "aprx.h"
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>


/* The ttyreader does read TTY ports into a big buffer, and then from there
   to packet frames depending on what is attached...  */


static struct serialport **ttys;
static int ttycount;		/* How many are defined ? */

static void ttyreader_linewrite(struct serialport *S); /* forward declaration */

#define TTY_OPEN_RETRY_DELAY_SECS 30


static int ttyreader_kissprocess(struct serialport *S)
{
	int i;
	int cmdbyte = S->rdline[0];
	int tncid = (cmdbyte >> 4) & 0x0F;

	/* --
	 * C0 00
	 * 82 A0 B4 9A 88 A4 60
	 * 9E 90 64 90 A0 9C 72
	 * 9E 90 64 A4 88 A6 E0
	 * A4 8C 9E 9C 98 B2 61
	 * 03 F0
	 * 21 36 30 32 39 2E 35 30 4E 2F 30 32 35 30 35 2E 34 33 45 3E 20 47 43 53 2D 38 30 31 20
	 * C0
	 * --
	 */

	/* printf("ttyreader_kissprocess()  cmdbyte=%02X len=%d ",cmdbyte,S->rdlinelen); */

	/* Ok, cmdbyte tells us something, and we should ignore the
	   frame if we don't know it... */

	if ((cmdbyte & 0x0F) != 0) {
		/* There should NEVER be any other value in the CMD bits
		   than 0  coming from TNC to host! */
		/* printf(" ..bad CMD byte\n"); */
		if (debug)
			printf("%ld\tTTY %s: Bad CMD byte on KISS frame: %02x\n", now, S->ttyname, cmdbyte);
		erlang_add(S->ttycallsign[tncid], ERLANG_DROP, S->rdlinelen, 1);	/* Account one packet */
		return -1;
	}


	/* Are we expecting Basic KISS ? */
	if (S->linetype == LINETYPE_KISS) {
	    if (S->ttycallsign[tncid] == NULL) {
	      /* D'OH!  received packet on multiplexer tncid without
		 callsign definition!  We discard this packet! */
	      if (debug > 0) {
		printf("%ld\tTTY %s: Bad TNCID on CMD byte on a KISS frame: %02x  No interface configured for it!\n", now, S->ttyname, cmdbyte);
	      }
	      erlang_add(S->ttycallsign[tncid], ERLANG_DROP, S->rdlinelen, 1);	/* Account one packet */
	      return -1;
	    }
	}

	/* Are we excepting BPQ "CRC" (XOR-sum of data) */
	if (S->linetype == LINETYPE_KISSBPQCRC) {
		/* TODO: in what conditions the "CRC" is calculated and when not ? */
		int xorsum = 0;

		if (S->ttycallsign[tncid] == NULL) {
		  /* D'OH!  received packet on multiplexer tncid without
		     callsign definition!  We discard this packet! */
		  if (debug > 0) {
		    printf("%ld\tTTY %s: Bad TNCID on CMD byte on a KISS frame: %02x  No interface configured for it!\n", now, S->ttyname, cmdbyte);
		  }
		  erlang_add(S->ttycallsign[tncid], ERLANG_DROP, S->rdlinelen, 1);	/* Account one packet */
		  return -1;
		}

		for (i = 1; i < S->rdlinelen; ++i)
			xorsum ^= S->rdline[i];
		xorsum &= 0xFF;
		if (xorsum != 0) {
			if (debug)
				printf("%ld\tTTY %s tncid %d: Received bad BPQCRC: %02x\n", now, S->ttyname, tncid, xorsum);
			erlang_add(S->ttycallsign[tncid], ERLANG_DROP, S->rdlinelen, 1);	/* Account one packet */
			return -1;
		}
		S->rdlinelen -= 1;	/* remove the sum-byte from tail */
		if (debug > 2)
			printf("%ld\tTTY %s tncid %d: Received OK BPQCRC frame\n", now, S->ttyname, tncid);
	}
	/* Are we expecting SMACK ? */
	if (S->linetype == LINETYPE_KISSSMACK) {

	    tncid &= 0x07;	/* Chop off top bit */

	    if (S->ttycallsign[tncid] == NULL) {
	      /* D'OH!  received packet on multiplexer tncid without
		 callsign definition!  We discard this packet! */
	      if (debug > 0) {
		printf("%ld\tTTY %s: Bad TNCID on CMD byte on a KISS frame: %02x  No interface configured for it!\n", now, S->ttyname, cmdbyte);
	      }
	      erlang_add(S->ttycallsign[tncid], ERLANG_DROP, S->rdlinelen, 1);	/* Account one packet */
	      return -1;
	    }

	    if ((cmdbyte & 0x8F) == 0x80) {
	        /* SMACK data frame */

		if (debug > 3)
		    printf("%ld\tTTY %s tncid %d: Received SMACK frame\n", now, S->ttyname, tncid);

		if (!(S->smack_subids & (1 << tncid))) {
		    if (debug)
			printf("%ld\t... marking received SMACK\n", now);
		}
		S->smack_subids |= (1 << tncid);

		/* It is SMACK frame -- KISS with CRC16 at the tail.
		   Now we ignore the TNC-id number field.
		   Verify the CRC.. */

		// Whole buffer including CMD-byte!
		if (crc16_calc(S->rdline, S->rdlinelen) != 0) {
			if (debug)
				printf("%ld\tTTY %s tncid %d: Received SMACK frame with invalid CRC\n",
				       now, S->ttyname, tncid);
			erlang_add(S->ttycallsign[tncid], ERLANG_DROP, S->rdlinelen, 1);	/* Account one packet */
			return -1;	/* The CRC was invalid.. */
		}

		S->rdlinelen -= 2;	/* Chop off the two CRC bytes */

	    } else if ((cmdbyte & 0x8F) == 0x00) {
	    	/*
		 * Expecting SMACK data, but got plain KISS data.
		 * Send a flow-rate limited probes to TNC to enable
		 * SMACK -- lets use 30 minutes window...
		 */


		S->smack_subids &= ~(1 << tncid); // Turn off the SMACK mode indication bit..

		if (debug > 2)
		    printf("%ld\tTTY %s tncid %d: Expected SMACK, got KISS.\n", now, S->ttyname, tncid);

		if (S->smack_probe[tncid] < now) {
		    uint8_t probe[4];
		    uint8_t kissbuf[12];
		    int kisslen;

		    probe[0] = cmdbyte | 0x80;  /* Make it into SMACK */
		    probe[1] = 0;

		    /* Convert the probe packet to KISS frame */
		    kisslen = kissencoder( kissbuf, sizeof(kissbuf),
					   &(probe[1]), 1, probe[0] );

		    /* Send probe message..  */
		    if (S->wrlen + kisslen < sizeof(S->wrbuf)) {
			/* There is enough space in writebuf! */

			memcpy(S->wrbuf + S->wrlen, kissbuf, kisslen);
			S->wrlen += kisslen;
			/* Flush it out..  and if not successfull,
			   poll(2) will take care of it soon enough.. */
			ttyreader_linewrite(S);

			S->smack_probe[tncid] = now + 30*60; /* 30 minutes */

			if (debug)
			    printf("%ld\tTTY %s tncid %d: Sending SMACK activation probe packet\n", now, S->ttyname, tncid);

		    }
		    /* Else no space to write ?  Huh... */
		}
	    } else {
		// Else...  there should be no other kind data frames
		if (debug)
		    printf("%ld\tTTY %s: Bad CMD byte on expected SMACK frame: %02x,  len=%d\n",
			   now, S->ttyname, cmdbyte, S->rdlinelen);
		erlang_add(S->ttycallsign[tncid], ERLANG_DROP, S->rdlinelen, 1);	/* Account one packet */
		return -1;
	    }
	}

	if (S->rdlinelen < 17) {
		/* 7+7+2 bytes of minimal AX.25 frame + 1 for KISS CMD byte */

		/* Too short frame.. */
		/* printf(" ..too short a frame for anything\n");  */
		erlang_add(S->ttycallsign[tncid], ERLANG_DROP, S->rdlinelen, 1);	/* Account one packet */
		return -1;
	}

	/* Valid AX.25 HDLC frame byte sequence is now at
	   S->rdline[1..S->rdlinelen-1]
	 */

	/* Send the frame to APRS-IS, return 1 if valid AX.25 UI message, does not
	   validate against valid APRS message rules... (TODO: it could do that too) */

	// The AX25_TO_TNC2 does validate the AX.25 packet,
	// converts it to "TNC2 monitor format" and sends it to
	// Rx-IGate functionality.  Returns non-zero only when
	// AX.25 header is OK, and packet is sane.

	erlang_add(S->ttycallsign[tncid], ERLANG_RX, S->rdlinelen, 1);	/* Account one packet */

	if (ax25_to_tnc2(S->interface[tncid], S->ttycallsign[tncid], tncid,
			 cmdbyte, S->rdline + 1, S->rdlinelen - 1)) {
		// The packet is valid per AX.25 header bit rules.

		/* Send the frame without cmdbyte to internal AX.25 network */
		if (S->netax25[tncid] != NULL)
			netax25_sendax25(S->netax25[tncid], S->rdline + 1, S->rdlinelen - 1);

	} else {
	  // The packet is not valid per AX.25 header bit rules
	  erlang_add(S->ttycallsign[tncid], ERLANG_DROP, S->rdlinelen, 1);	/* Account one packet */

	  if (aprxlogfile) {
	    FILE *fp = fopen(aprxlogfile, "a");
	    if (fp) {
	      char timebuf[60];
	      printtime(timebuf, sizeof(timebuf), now);

	      fprintf(fp, "%s ax25_to_tnc2(%s,len=%d) rejected the message\n", timebuf, S->ttycallsign[tncid], S->rdlinelen-1);
	      fclose(fp);
	    }
	  }
	}

	return 0;
}


/*
 *  ttyreader_getc()  -- pick one char ( >= 0 ) out of input buffer, or -1 if out of buffer
 */
static int ttyreader_getc(struct serialport *S)
{
	if (S->rdcursor >= S->rdlen) {	/* Out of data ? */
		if (S->rdcursor)
			S->rdcursor = S->rdlen = 0;
		/* printf("-\n"); */
		return -1;
	}

	/* printf(" %02X", 0xFF & S->rdbuf[S->rdcursor++]); */

	return (0xFF & S->rdbuf[S->rdcursor++]);
}


/*
 * ttyreader_pullkiss()  --  pull KISS (or KISS+CRC) frame, and call KISS processor
 */

static int ttyreader_pullkiss(struct serialport *S)
{
	/* printf("ttyreader_pullkiss()  rdlen=%d rdcursor=%d, state=%d\n",
	   S->rdlen, S->rdcursor, S->kissstate); fflush(stdout); */

	/* At incoming call there is at least one byte in between
	   S->rdcursor and S->rdlen  */

	/* Phases:
	   kissstate == 0: hunt for KISS_FEND, discard everything before it.
	   kissstate != 0: reading has globbed up preceding KISS_FENDs
	   ("HDLC flags") and the cursor is in front of a frame
	 */

	/* There are TNCs that use "shared flags" - only one FEND in between
	   data frames. */

	if (S->kissstate == KISSSTATE_SYNCHUNT) {
		/* Hunt for KISS_FEND, discard everything until then! */
		int c;
		for (;;) {
			c = ttyreader_getc(S);
			if (c < 0)
				return c;	/* Out of buffer, stay in state,
						   return latter when there is some
						   refill */
			if (c == KISS_FEND)	/* Found the sync-byte !  change state! */
				break;
		}
		S->kissstate = KISSSTATE_COLLECTING;
	}


	if (S->kissstate != KISSSTATE_SYNCHUNT) {
		/* Normal processing mode */

		int c;

		for (;;) {
			c = ttyreader_getc(S);
			if (c < 0)
				return c;	/* Out of input stream, exit now,
						   come back latter.. */

			/* printf(" %02X", c);
			   if (c == KISS_FEND) { printf("\n");fflush(stdout); }  */

			if (c == KISS_FEND) {
				/* Found end-of-frame character -- or possibly beginning..
				   This never exists in datastream except as itself. */

				if (S->rdlinelen > 0) {
					/* Non-zero sized frame  Process it away ! */
					ttyreader_kissprocess(S);
					S->kissstate =
						KISSSTATE_COLLECTING;
					S->rdlinelen = 0;
				}

				/* rdlinelen == 0 because we are receiving consequtive
				   FENDs, or just processed our previous frame.  Treat
				   them the same: discard this byte. */

				continue;
			}

			if (S->kissstate == KISSSTATE_KISSFESC) {

				/* We have some char, state switches to normal collecting */
				S->kissstate = KISSSTATE_COLLECTING;

				if (c == KISS_TFEND)
					c = KISS_FEND;
				else if (c == KISS_TFESC)
					c = KISS_FESC;
				else
					continue;	/* Accepted chars after KISS_FESC
							   are only TFEND and TFESC.
							   Others must be discarded. */

			} else {	/* Normal collection mode */

				if (c == KISS_FESC) {
					S->kissstate = KISSSTATE_KISSFESC;
					continue;	/* Back to top of the loop and continue.. */
				}

			}


			if (S->rdlinelen >= (sizeof(S->rdline) - 3)) {
				/* Too long !  Way too long ! */

				S->kissstate = KISSSTATE_SYNCHUNT;	/* Sigh.. discard it. */
				S->rdlinelen = 0;
				if (debug)
					printf("%ld\tTTY %s: Too long frame to be KISS..\n", now, S->ttyname);
				continue;
			}

			/* Put it on record store: */
			S->rdline[S->rdlinelen++] = c;
		}		/* .. for(..) loop of data collecting */

	}
	/* .. normal consumption mode ... */
	return 0;
}

/*
 *  ttyreader_pulltnc2()  --  process a line of text by calling
 *				TNC2 UI Monitor analyzer
 */

static int ttyreader_pulltnc2(struct serialport *S)
{
	const uint8_t *p;
	int addrlen = 0;
	p = memchr(S->rdline, ':', S->rdlinelen);
	if (p != NULL)
	  addrlen = (int)(p - S->rdline);

	erlang_add(S->ttycallsign[0], ERLANG_RX, S->rdlinelen, 1);	/* Account one packet */

	/* Send the frame to internal AX.25 network */
	/* netax25_sendax25_tnc2(S->rdline, S->rdlinelen); */

	/* S->rdline[] has text line without line ending CR/LF chars   */
	igate_to_aprsis(S->ttycallsign[0], 0, (char *) (S->rdline), addrlen, S->rdlinelen, 0);

	return 0;
}

#if 0
/*
 * ttyreader_pullaea()  --  process a line of text by calling
 * 			    AEA MONITOR 1 analyzer
 */

static int ttyreader_pullaea(struct serialport *S)
{
	int i;

	if (S->rdline[S->rdlinelen - 1] == ':') {
		/* Could this be the AX25 header ? */
		char *s = strchr(S->rdline, '>');
		if (s) {
			/* Ah yes, it well could be.. */
			strcpy(S->rdline2, S->rdline);
			return;
		}
	}

	/* FIXME: re-arrange the  S->rdline2  contained AX25 address tokens 
	   and flags..

	   perl code:
	   @addrs = split('>', $rdline2);
	   $out = shift @addrs; # pop first token in sequence
	   $out .= '>';
	   $out .= pop @addrs;  # pop last token in sequence
	   foreach $a (@addrs) { # rest of the tokens in sequence, if any
	   $out .= ',' . $a;
	   }
	   # now $out has address data in TNC2 sequence.
	 */

	/* printf("%s%s\n", S->rdline2, S->rdline); fflush(stdout); */

	return 0;
}
#endif


/*
 *  ttyreader_pulltext()  -- process a line of text from the serial port..
 */

static int ttyreader_pulltext(struct serialport *S)
{
	int c;

	for (;;) {

		c = ttyreader_getc(S);
		if (c < 0)
			return c;	/* Out of input.. */

		/* S->kissstate != 0: read data into S->rdline,
		   == 0: discard data until CR|LF.
		   Zero-size read line is discarded as well
		   (only CR|LF on input frame)  */

		if (S->kissstate == KISSSTATE_SYNCHUNT) {
			/* Looking for CR or LF.. */
			if (c == '\n' || c == '\r')
				S->kissstate = KISSSTATE_COLLECTING;

			S->rdlinelen = 0;
			continue;
		}

		/* Now: (S->kissstate != KISSSTATE_SYNCHUNT)  */

		if (c == '\n' || c == '\r') {
			/* End of line seen! */
			if (S->rdlinelen > 0) {

				/* Non-zero-size string, put terminating 0 byte on it. */
				S->rdline[S->rdlinelen] = 0;

				/* .. and process it depending ..  */

				if (S->linetype == LINETYPE_TNC2) {
					ttyreader_pulltnc2(S);
#if 0
				} else {	/* .. it is LINETYPE_AEA ? */
					ttyreader_pullaea(S);
#endif
				}
			}
			S->rdlinelen = 0;
			continue;
		}

		/* Now place the char in the linebuffer, if there is space.. */
		if (S->rdlinelen >= (sizeof(S->rdline) - 3)) {	/* Too long !  Way too long ! */
			S->kissstate = KISSSTATE_SYNCHUNT;	/* Sigh.. discard it. */
			S->rdlinelen = 0;
			continue;
		}

		/* Put it on line store: */
		S->rdline[S->rdlinelen++] = c;

	}			/* .. input loop */

	return 0;		/* not reached */
}



/*
 *  ttyreader_kisswrite()  -- write out buffered data
 */
void ttyreader_kisswrite(struct serialport *S, const int tncid, const uint8_t *ax25raw, const int ax25rawlen)
{
	int i, len, ssid;
	char kissbuf[2300];

	if (debug) printf("ttyreader_kisswrite(->%s, axlen=%d)", S->ttycallsign[tncid], ax25rawlen);

	if ((S->linetype != LINETYPE_KISS) && (S->linetype != LINETYPE_KISSSMACK) &&
	    (S->linetype != LINETYPE_KISSBPQCRC)) {
		if (debug)
		  printf("WARNING: WRITING KISS FRAMES ON SERIAL/TCP LINE OF NO KISS TYPE IS UNSUPPORTED!\n");
		return;
	}


	if ((S->wrlen == 0) || (S->wrlen > 0 && S->wrcursor >= S->wrlen)) {
		S->wrlen = S->wrcursor = 0;
	} else {
	  /* There is some data in between wrcursor and wrlen */
	  len = S->wrlen - S->wrcursor;
	  if (len > 0) {
	    i = write(S->fd, S->wrbuf + S->wrcursor, len);
	  } else
	    i = 0;
	  if (i > 0) {		/* wrote something */
	    S->wrcursor += i;
	    len = S->wrlen - S->wrcursor;
	    if (len == 0) {
	      S->wrcursor = S->wrlen = 0;	/* wrote all ! */
	    } else {
	      /* compact the buffer a bit */
	      memcpy(S->wrbuf, S->wrbuf + S->wrcursor, len);
	      S->wrcursor = 0;
	      S->wrlen = len;
	    }
	  }
	}

	ssid = (tncid << 4) | ((S->linetype == LINETYPE_KISSSMACK) ? 0x80 : 0x00);
	len = kissencoder( kissbuf, sizeof(kissbuf), ax25raw, ax25rawlen, ssid );

	// Will the KISS encoded frame fit in the link buffer?
	if ((S->wrlen + len) < sizeof(S->wrbuf)) {
		memcpy(S->wrbuf + S->wrlen, kissbuf, len);
		S->wrlen += len;
		erlang_add(S->ttycallsign[tncid], ERLANG_TX, ax25rawlen, 1);

		if (debug)
		  printf(" .. put %d bytes of KISS frame on IO buffer\n",len);
	} else {
		// No fit!
		if (debug)
		  printf(" .. %d bytes of KISS frame did not fit on IO buffer\n",len);
		return;
	}

	// Try to write it immediately
	len = S->wrlen - S->wrcursor;
	if (len > 0)
	  i = write(S->fd, S->wrbuf + S->wrcursor, len);
	else
	  i = 0;
	if (i > 0) {		/* wrote something */
		S->wrcursor += i;
		len = S->wrlen - S->wrcursor; /* all done? */
		if (len == 0) {
			S->wrcursor = S->wrlen = 0;	/* wrote all ! */
		} else {
			/* compact the buffer a bit */
			memcpy(S->wrbuf, S->wrbuf + S->wrcursor, len);
			S->wrcursor = 0;
			S->wrlen = len;
		}
	}
}

/*
 *  ttyreader_linewrite()  -- write out buffered data
 */
static void ttyreader_linewrite(struct serialport *S)
{
	int i, len;

	if ((S->wrlen == 0) || (S->wrlen > 0 && S->wrcursor >= S->wrlen)) {
		S->wrlen = S->wrcursor = 0;	/* already all written */
		return;
	}

	/* Now there is some data in between wrcursor and wrlen */

	len = S->wrlen - S->wrcursor;
	if (len > 0)
	  i = write(S->fd, S->wrbuf + S->wrcursor, len);
	else
	  i = 0;
	if (i > 0) {		/* wrote something */
		S->wrcursor += i;
		len = S->wrlen - S->wrcursor;
		if (len == 0) {
			S->wrcursor = S->wrlen = 0;	/* wrote all ! */
		} else {
			/* compact the buffer a bit */
			memcpy(S->wrbuf, S->wrbuf + S->wrcursor, len);
			S->wrcursor = 0;
			S->wrlen = len;
		}
	}
}


/*
 *  ttyreader_lineread()  --  read what there is into our buffer,
 *			      and process the buffer..
 */

static void ttyreader_lineread(struct serialport *S)
{
	int i;

	int rdspace = sizeof(S->rdbuf) - S->rdlen;

	if (S->rdcursor > 0) {
		/* Read-out cursor is not at block beginning,
		   is there unread data too ?  */
		if (S->rdlen > S->rdcursor) {
			/* Uh..  lets move buffer down a bit,
			   to make room for more to the end.. */
			memcpy(S->rdbuf, S->rdbuf + S->rdcursor,
			       S->rdlen - S->rdcursor);
			S->rdlen = S->rdlen - S->rdcursor;
		} else
			S->rdlen = 0;	/* all processed, mark its size zero */
		/* Cursor to zero, rdspace recalculated */
		S->rdcursor = 0;

		/* recalculate */
		rdspace = sizeof(S->rdbuf) - S->rdlen;
	}

	if (rdspace > 0) {	/* We have room to read into.. */
		i = read(S->fd, S->rdbuf + S->rdlen, rdspace);
		if (i == 0) {	/* EOF ?  USB unplugged ? */
			close(S->fd);
			S->fd = -1;
			S->wait_until = now + TTY_OPEN_RETRY_DELAY_SECS;
			if (debug)
				printf("%ld\tTTY %s EOF - CLOSED, WAITING %d SECS\n", now, S->ttyname, TTY_OPEN_RETRY_DELAY_SECS);
			return;
		}
		if (i < 0)	/* EAGAIN or whatever.. */
			return;

		/* Some data has been accumulated ! */
		S->rdlen += i;
		S->last_read_something = now;
	}

	/* Done reading, maybe.  Now processing.
	   The pullXX does read up all input, and does
	   however many frames there are in, and pauses
	   when there is no enough input data for a full
	   frame/line/whatever.
	 */

	if (S->linetype == LINETYPE_KISS ||
	    S->linetype == LINETYPE_KISSSMACK) {

		ttyreader_pullkiss(S);


	} else if (S->linetype == LINETYPE_TNC2
#if 0
		   || S->linetype == LINETYPE_AEA
#endif
		) {

		ttyreader_pulltext(S);

	} else {
		close(S->fd);	/* Urgh ?? Bad linetype value ?? */
		S->fd = -1;
		S->wait_until = now + TTY_OPEN_RETRY_DELAY_SECS;
	}

	/* Consumed something, and our read cursor is not in the beginning ? */
	if (S->rdcursor > 0 && S->rdcursor < S->rdlen) {
		/* Compact the input buffer! */
		memcpy(S->rdbuf, S->rdbuf + S->rdcursor,
		       S->rdlen - S->rdcursor);
	}
	S->rdlen -= S->rdcursor;
	S->rdcursor = 0;
}


/*
 * ttyreader_linesetup()  --  open and configure the serial port
 */

static void ttyreader_linesetup(struct serialport *S)
{
	int i;

	S->wait_until = 0;	/* Zero it just to be safe */

	S->wrlen = S->wrcursor = 0;	/* init them at first */

	if (memcmp(S->ttyname, "tcp!", 4) != 0) {

		S->fd = open(S->ttyname, O_RDWR | O_NOCTTY, 0);

		if (debug)
			printf("%ld\tTTY %s OPEN - fd=%d - ",
			       now, S->ttyname, S->fd);
		if (S->fd < 0) {	/* Urgh.. an error.. */
			S->wait_until = now + TTY_OPEN_RETRY_DELAY_SECS;
			if (debug)
				printf("FAILED, WAITING %d SECS\n",
				       TTY_OPEN_RETRY_DELAY_SECS);
			return;
		}
		if (debug)
			printf("OK\n");

		/* Set attributes */
		aprx_cfmakeraw(&S->tio, 1); /* hw-flow on */
		i = tcsetattr(S->fd, TCSAFLUSH, &S->tio);

		if (i < 0) {
		  printf("%ld\tTCSETATTR failed; errno=%d\n",
			       now, errno);
			close(S->fd);
			S->fd = -1;
			S->wait_until = now + TTY_OPEN_RETRY_DELAY_SECS;
			return;
		}
		/* FIXME: ??  Set baud-rates ?
		   Used system (Linux) has them in   'struct termios'  so they
		   are now set, but other systems may have different ways..
		 */

		/* Flush buffers once again. */
		i = tcflush(S->fd, TCIOFLUSH);

		/* change the file handle to non-blocking */
		fd_nonblockingmode(S->fd);

		for (i = 0; i < 16; ++i) {
		  if (S->initstring[i] != NULL) {
		    memcpy(S->wrbuf + S->wrlen, S->initstring[i], S->initlen[i]);
		    S->wrlen += S->initlen[i];
		  }
		}

		/* Flush it out..  and if not successfull,
		   poll(2) will take care of it soon enough.. */
		ttyreader_linewrite(S);

	} else {		/* socket connection to remote TTY.. */
		/*   "tcp!hostname-or-ip!port!opt-parameters" */
		char *par = strdup(S->ttyname);
		char *host = NULL, *port = NULL, *opts = NULL;
		struct addrinfo req, *ai;
		int i;

		if (debug)
			printf("socket connect() preparing: %s\n", par);

		for (;;) {
			host = strchr(par, '!');
			if (host)
				++host;
			else
				break;	/* Found no '!' ! */
			port = strchr(host, '!');
			if (port)
				*port++ = 0;
			else
				break;	/* Found no '!' ! */
			opts = strchr(port, '!');
			if (opts)
				*opts++ = 0;
			break;
		}

		if (!port) {
			/* Still error condition.. no port data */
		}

		memset(&req, 0, sizeof(req));
		req.ai_socktype = SOCK_STREAM;
		req.ai_protocol = IPPROTO_TCP;
		req.ai_flags = 0;
#if 1
		req.ai_family = AF_UNSPEC;	/* IPv4 and IPv6 are both OK */
#else
		req.ai_family = AF_INET;	/* IPv4 only */
#endif
		ai = NULL;

		i = getaddrinfo(host, port, &req, &ai);

		if (ai) {
			S->fd = socket(ai->ai_family, SOCK_STREAM, 0);
			if (S->fd >= 0) {

				fd_nonblockingmode(S->fd);

				i = connect(S->fd, ai->ai_addr,
					    ai->ai_addrlen);
				if ((i != 0) && (errno != EINPROGRESS)) {
					/* non-blocking connect() yields EINPROGRESS,
					   anything else and we fail entirely...      */
					if (debug)
						printf("ttyreader socket connect call failed: %d : %s\n", errno, strerror(errno));
					close(S->fd);
					S->fd = -1;
				}
			}

			freeaddrinfo(ai);
		}
		free(par);
	}

	S->last_read_something = now;	/* mark the timeout for future.. */

	S->rdlen = S->rdcursor = S->rdlinelen = 0;
	S->kissstate = 0;	/* Zero it, whatever protocol we actually use
				   will consider it as 'hunt for sync' state. */

	memset( S->smack_probe, 0, sizeof(S->smack_probe) );
	S->smack_subids = 0;
}

/*
 *  ttyreader_init()
 */

void ttyreader_init(void)
{
	/* nothing.. */
}



/*
 *  ttyreader_prepoll()  --  prepare system for next round of polling
 */

int ttyreader_prepoll(struct aprxpolls *app)
{
	int idx = 0;		/* returns number of *fds filled.. */
	int i;
	struct serialport *S;
	struct pollfd *pfd;

	for (i = 0; i < ttycount; ++i) {
		S = ttys[i];
		if (!S->ttyname)
			continue;	/* No name, no look... */
		if (S->fd < 0) {
			/* Not an open TTY, but perhaps waiting ? */
			if ((S->wait_until != 0) && (S->wait_until > now)) {
				/* .. waiting for future! */
				if (app->next_timeout > S->wait_until)
					app->next_timeout = S->wait_until;
				/* .. but only until our timeout,
				   if it is sooner than global one. */
				continue;	/* Waiting on this one.. */
			}

			/* Waiting or not, FD is not open, and deadline is past.
			   Lets try to open! */

			ttyreader_linesetup(S);

		}
		/* .. No open FD */
		/* Still no open FD ? */
		if (S->fd < 0)
			continue;

		/* FD is open, check read/idle timeout ... */
		if ((S->read_timeout > 0) &&
		    (now > (S->last_read_something + S->read_timeout))) {
			if (debug)
			  printf("%ld\tRead timeout on %s; %d seconds w/o input. fd=%d\n",
				 now, S->ttyname, S->read_timeout, S->fd);
			close(S->fd);	/* Close and mark for re-open */
			S->fd = -1;
			S->wait_until = now + TTY_OPEN_RETRY_DELAY_SECS;
			continue;
		}

		/* FD is open, lets mark it for poll read.. */
		pfd = aprxpolls_new(app);
		pfd->fd = S->fd;
		pfd->events = POLLIN | POLLPRI;
		pfd->revents = 0;
		if (S->wrlen > 0 && S->wrlen > S->wrcursor)
			pfd->events |= POLLOUT;

		++idx;
	}
	return idx;
}

/*
 *  ttyreader_postpoll()  -- Done polling, what happened ?
 */

int ttyreader_postpoll(struct aprxpolls *app)
{
	int idx, i;

	struct serialport *S;
	struct pollfd *P;
	for (idx = 0, P = app->polls; idx < app->pollcount; ++idx, ++P) {

		if (!(P->revents & (POLLIN | POLLPRI | POLLERR | POLLHUP)))
			continue;	/* No read event we are interested in... */

		for (i = 0; i < ttycount; ++i) {
			S = ttys[i];
			if (S->fd != P->fd)
				continue;	/* Not this one ? */
			/* It is this one! */

			if (P->revents & POLLOUT)
				ttyreader_linewrite(S);

			ttyreader_lineread(S);
		}
	}

	return 0;
}

/*
 * Make a pre-existing termios structure into "raw" mode: character-at-a-time
 * mode with no characters interpreted, 8-bit data path.
 */
void
aprx_cfmakeraw(t, f)
	struct termios *t;
{

	t->c_iflag &= ~(IMAXBEL|IXOFF|INPCK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL|IXON|IGNPAR);
	t->c_iflag |= IGNBRK;

	t->c_oflag &= ~OPOST;
	if (f) {
	  t->c_oflag |= CRTSCTS;
	} else {
	  t->c_oflag &= ~CRTSCTS;
	}

	t->c_lflag &= ~(ECHO|ECHOE|ECHOK|ECHONL|ICANON|ISIG|IEXTEN|NOFLSH|TOSTOP|PENDIN);
	t->c_cflag &= ~(CSIZE|PARENB);
	t->c_cflag |= CS8|CREAD;
	t->c_cc[VMIN] = 80;
	t->c_cc[VTIME] = 3;
}

struct serialport *ttyreader_new(void)
{
	struct serialport *tty = malloc(sizeof(*tty));
	int baud = B1200;

	memset(tty, 0, sizeof(*tty));

	tty->fd = -1;
	tty->wait_until = now - 1;	/* begin opening immediately */
	tty->last_read_something = now;	/* well, not really.. */
	tty->linetype  = LINETYPE_KISS;	/* default */
	tty->kissstate = KISSSTATE_SYNCHUNT;

	tty->ttyname = NULL;


	/* setup termios parameters for this line.. */
	aprx_cfmakeraw(&tty->tio, 0);
	tty->tio.c_cc[VMIN] = 80;	/* pick at least one char .. */
	tty->tio.c_cc[VTIME] = 3;	/* 0.3 seconds timeout - 36 chars @ 1200 baud */
	tty->tio.c_cflag |= (CREAD | CLOCAL);

	cfsetispeed(&tty->tio, baud);
	cfsetospeed(&tty->tio, baud);

	return tty;
}

void ttyreader_parse_ttyparams(struct configfile *cf, struct serialport *tty, char *str)
{
	int i;
	speed_t baud;
	int tncid   = 0;
	char *param1 = 0;


	/* FIXME: analyze correct serial port data and parity format settings,
	   now hardwired to 8-n-1 -- does not work without for KISS anyway.. */
	
	config_STRLOWER(str);	/* until end of line */

	/* Optional parameters */
	while (*str != 0) {
		param1 = str;
		str = config_SKIPTEXT(str, NULL);
		str = config_SKIPSPACE(str);

		if (debug)
		  printf(" .. param='%s'",param1);

		/* See if it is baud-rate ? */
		i = atol(param1);	/* serial port speed - baud rate */
		baud = B1200;
		switch (i) {
		case 1200:
			baud = B1200;
			break;
		case 2400:
			baud = B2400;
			break;
		case 4800:
			baud = B4800;
			break;
		case 9600:
			baud = B9600;
			break;
		case 19200:
			baud = B19200;
			break;
		case 38400:
			baud = B38400;
			break;
		default:
			i = -1;
			break;
		}
		if (baud != B1200) {
			cfsetispeed(&tty->tio, baud);
			cfsetospeed(&tty->tio, baud);
		}

		/* Note:  param1  is now lower-case string */

		if (i > 0) {
			;
		} else if (strcmp(param1, "8n1") == 0) {
			/* default behaviour, ignore */
		} else if (strcmp(param1, "kiss") == 0) {
			tty->linetype = LINETYPE_KISS;	/* plain basic KISS */

		} else if (strcmp(param1, "xorsum") == 0) {
			tty->linetype = LINETYPE_KISSBPQCRC;	/* KISS with BPQ "CRC" */
		} else if (strcmp(param1, "bpqcrc") == 0) {
			tty->linetype = LINETYPE_KISSBPQCRC;	/* KISS with BPQ "CRC" */

		} else if (strcmp(param1, "smack") == 0) {
			tty->linetype = LINETYPE_KISSSMACK;	/* KISS with SMACK / CRC16 */
		} else if (strcmp(param1, "crc16") == 0) {
			tty->linetype = LINETYPE_KISSSMACK;	/* KISS with SMACK / CRC16 */

		} else if (strcmp(param1, "poll") == 0) {
			/* FIXME: Some systems want polling... */

		} else if (strcmp(param1, "callsign") == 0 ||
			   strcmp(param1, "alias") == 0) {
			param1 = str;
			str = config_SKIPTEXT(str, NULL);
			str = config_SKIPSPACE(str);
			config_STRUPPER(param1);
			tty->ttycallsign[tncid] = strdup(param1);

			tty->netax25[tncid] = netax25_open(param1);

			/* Use side-effect: this defines the tty into
			   erlang accounting */

			erlang_set(param1, /* Heuristic constant for max channel capa.. */ (int) ((1200.0 * 60) / 8.2));

		} else if (strcmp(param1, "timeout") == 0) {
			param1 = str;
			str = config_SKIPTEXT(str, NULL);
			str = config_SKIPSPACE(str);
			tty->read_timeout = atol(param1);

		} else if (strcmp(param1, "tncid") == 0) {
			param1 = str;
			str = config_SKIPTEXT(str, NULL);
			str = config_SKIPSPACE(str);
			tncid = atoi(param1);
			if (tncid < 0 || tncid > 15)
				tncid = 0;

		} else if (strcmp(param1, "tnc2") == 0) {
			tty->linetype = LINETYPE_TNC2;	/* TNC2 monitor */

		} else if (strcmp(param1, "initstring") == 0) {
			int parlen;
			param1 = str;
			str = config_SKIPTEXT(str, &parlen);
			str = config_SKIPSPACE(str);
			tty->initlen[tncid]    = parlen;
			tty->initstring[tncid] = malloc(parlen);
			memcpy(tty->initstring[tncid], param1, parlen);

			if (debug)
			  printf("initstring len=%d\n",parlen);
		} else {
		  printf("%s:%d Unknown sub-keyword on 'radio' configuration: '%s'\n",
			 cf->name, cf->linenum, param1);
		}
	}
	if (debug) printf("\n");
}

void ttyreader_register(struct serialport *tty)
{
	/* Grow the array as is needed.. - this is array of pointers,
	   not array of blocks so that memory allocation does not
	   grow into way too big chunks. */
	ttys = realloc(ttys, sizeof(void *) * (ttycount + 1));
	ttys[ttycount++] = tty;
}

const char *ttyreader_serialcfg(struct configfile *cf, char *param1, char *str)
{				/* serialport /dev/ttyUSB123   19200  8n1   {KISS|TNC2|AEA|..}  */
	struct serialport *tty;

	/*
	   radio serial /dev/ttyUSB123  [19200 [8n1]]  KISS
	   radio tcp 12.34.56.78 4001 KISS

	 */

	if (*param1 == 0)
		return "Bad mode keyword";
	if (*str == 0)
		return "Bad tty-name/param";

	tty = ttyreader_new();
	ttyreader_register(tty);

	if (strcmp(param1, "serial") == 0) {
		/* New style! */
		free((char *) (tty->ttyname));

		param1 = str;
		str = config_SKIPTEXT(str, NULL);
		str = config_SKIPSPACE(str);

		tty->ttyname = strdup(param1);

		if (debug)
			printf(".. new style serial:  '%s' '%s'..\n",
			       tty->ttyname, str);

	} else if (strcmp(param1, "tcp") == 0) {
		/* New style! */
		int len;
		char *host, *port;

		free((char *) (tty->ttyname));

		host = str;
		str = config_SKIPTEXT(str, NULL);
		str = config_SKIPSPACE(str);

		port = str;
		str = config_SKIPTEXT(str, NULL);
		str = config_SKIPSPACE(str);

		if (debug)
			printf(".. new style tcp!:  '%s' '%s' '%s'..\n",
			       host, port, str);

		len = strlen(host) + strlen(port) + 8;

		tty->ttyname = malloc(len);
		sprintf((char *) (tty->ttyname), "tcp!%s!%s!", host, port);

	}

	ttyreader_parse_ttyparams( cf, tty, str);
	return NULL;
}

