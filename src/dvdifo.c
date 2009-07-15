/*
	dvdauthor -- generation of IFO and BUP files
*/
/*
 * Copyright (C) 2002 Scott Smith (trckjunky@users.sourceforge.net)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307
 * USA
 */

#include "config.h"

#include "compat.h"

#include <assert.h>

#include "dvdauthor.h"
#include "da-internal.h"


#define BIGWRITEBUFLEN (16*2048)
static unsigned char bigwritebuf[BIGWRITEBUFLEN];

static void nfwrite(const void *ptr,size_t len,FILE *h)
/* writes to h, or turns into a noop if h is null. */
{
    if( h )
        fwrite(ptr,len,1,h);
}

static const struct vobuinfo *globalfindvobu(const struct pgc *ch,int pts)
{
    int s,c,ci;

    for( s=0; s<ch->numsources; s++ ) {
        const struct source *sc=ch->sources[s];
        for( c=0; c<sc->numcells; c++ ) {
            const struct cell *cl=&sc->cells[c];
            int span=0;
            int fv=findcellvobu(sc->vob,cl->scellid);

            if( pts < 0 )
                return &sc->vob->vi[fv];

            for( ci=cl->scellid; ci<cl->ecellid; ci++ )
                span+=getcellpts(sc->vob,ci);
            if( pts<span ) {
                int r=findvobu(sc->vob,pts+sc->vob->vi[fv].sectpts[0],
                               fv,sc->vob->numvobus-1);
                return &sc->vob->vi[r];
            }
            pts-=span;
        }
    }
    // return last vob
    // if( ch->numsources ) {
    // struct vob *s=ch->sources[ch->numsources-1];
    // return &s->vi[s->numvobus-1];
    // }
    return 0;
}

static int getvoblen(const struct vobgroup *va)
{
    int i;

    for( i=va->numvobs-1; i>=0; i-- )
        if( va->vobs[i]->numvobus )
            return va->vobs[i]->vi[va->vobs[i]->numvobus-1].lastsector+1;
    return 0;
}

static unsigned int getptssec(const struct vobgroup *va,int nsec)
/* converts nsec seconds to clock ticks adjusted for the video frame rate. */
{
    return nsec*getratedenom(va);
}

static unsigned int findptssec(const struct vobgroup *va,int pts)
/* converts pts frame-rate-adjusted clock ticks to seconds. */
{
    return pts/getratedenom(va);
}

static int numsec(const struct pgcgroup *va,int c)
{
    // we subtract 1 because there is a bug if getptsspan() returns
    // an exact multiple of 90090*units; if so, then the last entry of the
    // TMAPT table cannot be properly computed, because that entry will have
    // fallen off the end of the VOBU table
    return findptssec(va->vg,getptsspan(va->pgcs[c])-1);
}

static int secunit(int ns)
{
    const int maxunits=2040;

    if(!ns) return 1;
    return (ns+maxunits-1)/maxunits;
}

static int tmapt_block_size(const struct pgcgroup *va,int pgc)
{
    int v=numsec(va,pgc);
    v=v/secunit(v);
    return v*4+4;
}

static int sizeTMAPT(const struct pgcgroup *va)
{
    int s=0,i;
    for( i=0; i<va->numpgcs; i++ )
        s+=tmapt_block_size(va,i);
    return s+va->numpgcs*4+8;
}

static int numsectTMAPT(const struct pgcgroup *va)
{
    return (sizeTMAPT(va)+2047)/2048;
}

static void CreateTMAPT(FILE *h,const struct pgcgroup *va)
{
    int i,mapblock;
    unsigned char buf[8];

    write2(buf,va->numpgcs);
    write2(buf+2,0);
    write4(buf+4,sizeTMAPT(va)-1);
    nfwrite(buf,8,h);

    mapblock=8+4*va->numpgcs;
    for( i=0; i<va->numpgcs; i++ ) {
        write4(buf,mapblock);
        nfwrite(buf,4,h);
        mapblock+=tmapt_block_size(va,i);
    }

    for( i=0; i<va->numpgcs; i++ ) {
        int numtmapt=numsec(va,i), ptsbase, j;
        int units=secunit(numtmapt);
        const struct pgc *p=va->pgcs[i];

        numtmapt/=units;
        buf[0]=units;
        buf[1]=0;
        write2(buf+2,numtmapt);
        nfwrite(buf,4,h);
        if( numtmapt>0 ) {
            const struct vobuinfo *vobu1;
            // I don't know why I ever did this
            // ptsbase=-getframepts(va->vg);
            ptsbase=0; // this matches Bullitt
            vobu1=globalfindvobu(p,ptsbase+getptssec(va->vg,units));
            for( j=0; j<numtmapt; j++ ) {
                const struct vobuinfo *vobu2=globalfindvobu(p,ptsbase+getptssec(va->vg,(j+2)*units));
                write4(buf,vobu1->sector);
                if( !vobu2 || vobu1->vobcellid!=vobu2->vobcellid )
                    buf[0]|=0x80;
                nfwrite(buf,4,h);
                vobu1=vobu2;
            }
        }
    }

    i=(-sizeTMAPT(va))&2047;
    if( i ) {
        memset(buf,0,8);
        while(i>=8) {
            nfwrite(buf,8,h);
            i-=8;
        }
        if( i )
            nfwrite(buf,i,h);
    }
}

static int numsectVOBUAD(const struct vobgroup *va)
/* returns the number of sectors a VOBU_ADMAP will take up. */
{
    int nv=0, i;

    for( i=0; i<va->numvobs; i++ )
        nv+=va->vobs[i]->numvobus;

    return (4+nv*4+2047)/2048;
}

static int CreateCallAdr(FILE *h,const struct vobgroup *va)
/* outputs a VMGM_C_ADT, VTSM_C_ADT or VTS_C_ADT structure containing pointers to all cells. */
{
    unsigned char *buf=bigwritebuf;
    int i,p,k;

    memset(buf,0,BIGWRITEBUFLEN);
    p=8;
    for( k=0; k<va->numvobs; k++ ) {
        const struct vob *c=va->vobs[k];
        for( i=0; i<c->numvobus; i++ ) {
            if( !i || c->vi[i].vobcellid!=c->vi[i-1].vobcellid ) { /* starting a new cell */
                if( i ) {
                    write4(buf+p+8,c->vi[i-1].lastsector); /* ending sector within VOB in previous entry */
                    p+=12;
                }
                write2(buf+p,c->vi[i].vobcellid>>8); /* VOBidn */
                buf[p+2]=c->vi[i].vobcellid; /* CELLidn */
                write4(buf+p+4,c->vi[i].sector); /* starting sector within VOB */
            }
        }
        write4(buf+p+8,c->vi[i-1].lastsector); /* ending sector within VOB in last entry */
        p+=12;
    }
    write4(buf+4,p-1);
    // first 2 bytes of C_ADT contains number of vobs
    write2(buf,va->numvobs);
    assert(p<=BIGWRITEBUFLEN);
    p=(p+2047)&(-2048); /* round up to whole sectors */
    nfwrite(buf,p,h);
    return p/2048; /* nr sectors written */
}

static void CreateVOBUAD(FILE *h,const struct vobgroup *va)
/* outputs a VOBU_ADMAP structure. */
{
    int i,j,nv;
    unsigned char buf[16];

    nv=0;
    for( i=0; i<va->numvobs; i++ )
        nv+=va->vobs[i]->numvobus;

    write4(buf,nv*4+3);
    nfwrite(buf,4,h);
    for( j=0; j<va->numvobs; j++ ) {
        const struct vob *p=va->vobs[j];
        for( i=0; i<p->numvobus; i++ ) {
            write4(buf,p->vi[i].sector);
            nfwrite(buf,4,h);
        }
    }
    i=(-(4+nv*4))&2047;
    if( i ) {
        memset(buf,0,16);
        while(i>=16) {
            nfwrite(buf,16,h);
            i-=16;
        }
        if( i )
            nfwrite(buf,i,h);
    }
}

static int Create_PTT_SRPT(FILE *h,const struct pgcgroup *t)
/* creates the VTS_PTT_SRPT containing pointers to tables for each title. */
{
    unsigned char *buf=bigwritebuf;
    int i,j,p;

    memset(buf,0,BIGWRITEBUFLEN);
    write2(buf,t->numpgcs); // # of titles
    p=8+t->numpgcs*4; /* start generating VTS_PTT entries here */
    assert(p<=2048); // need to make sure all the pgc pointers fit in the first sector because of dvdauthor.c:ScanIfo
    for( j=0; j<t->numpgcs; j++ ) {
        const struct pgc *pgc=t->pgcs[j];
        int pgm=1,k;

        write4(buf+8+j*4,p); /* offset to VTS_PTT for title */
        for( i=0; i<pgc->numsources; i++ ) /* generate the associated VTS_PTT entries */
            for( k=0; k<pgc->sources[i]->numcells; k++ ) {
                const struct cell *c=&pgc->sources[i]->cells[k];
                if( c->scellid!=c->ecellid )
                    switch(c->ischapter ) {
                    case 1: /* chapter & program */
                        buf[1+p]=j+1; /* PGCN low byte */
                        buf[3+p]=pgm; /* PGN low byte */
                        p+=4;
				  /* fallthru */
                    case 2: /* program only */
                        pgm++;
                    }
            }
        
    }
    write4(buf+4,p-1); /* end address (last byte of last VTS_PTT) */
    assert(p<=BIGWRITEBUFLEN);
    p=(p+2047)&(-2048); /* round up to next whole sector */
    nfwrite(buf,p,h);
    return p/2048; /* nr sectors generated */
}

static int Create_TT_SRPT(FILE *h,const struct toc_summary *ts,int vtsstart /* starting sector for VTS */)
/* creates a TT_SRPT structure containing pointers to all the titles on the disc. */
{
    unsigned char *buf=bigwritebuf;
    int i,j,k,p,tn;

    memset(buf,0,BIGWRITEBUFLEN);

    j=vtsstart;
    tn=0;
    p=8;
    for( i=0; i<ts->numvts; i++ ) {
        for( k=0; k<ts->vts[i].numtitles; k++ ) {
            buf[0 + p]=0x3c; /* title type = jump/link/call may be found in all places */
            buf[1 + p]=0x1; /* number of angles always 1 for now */
            write2(buf+2+p,ts->vts[i].numchapters[k]); /* number of chapters (PTTs) */
            buf[6 + p]=i+1; /* video titleset number, VTSN */
            buf[7 + p]=k+1; /* title nr within VTS, VTS_TTN */
            write4(buf+8+p,j); // start sector for VTS
            tn++;
            p+=12;
        }
        j+=ts->vts[i].numsectors;
    }
    write2(buf,tn); // # of titles
    write4(buf+4,p-1); /* end address (last byte of last entry) */

    assert(p<=BIGWRITEBUFLEN);
    p=(p+2047)&(-2048); /* round up to next whole sector */
    nfwrite(buf,p,h);
    return p/2048; /* nr sectors generated */
}

static void BuildAVInfo(unsigned char *buf,const struct vobgroup *va)
/* builds the part of the IFO structure from offset 0x100 (VMGM, VTSM) and 0x200 (VTS) onwards. */
{
    int i;
    static int widescreen_bits[4]={0,0x100,0x200,2}; // VW_NONE, VW_NOLETTERBOX, VW_NOPANSCAN, VW_CROP

    write2(buf,
           (va->vd.vmpeg==2?0x4000:0)
           |widescreen_bits[va->vd.vwidescreen]
           |(va->vd.vformat==VF_PAL?0x1000:0)
           |(va->vd.vaspect==VA_16x9?0xc00:0x300) // if 16:9, set aspect flag; if 4:3 set noletterbox/nopanscan
           |((va->vd.vcaption&1)?0x80:0) /* caption=field1 (line-21 closed-captioning, NTSC only) */
           |((va->vd.vcaption&2)?0x40:0) /* caption=field2 (line-21 closed-captioning, NTSC only) */
           |((va->vd.vres-1)<<3)); /* resolution code */
		/* bit rate always VBR, letterbox-cropped unset, PAL film flag unset for now */
    buf[3]=va->numaudiotracks; /* nr audio streams, low byte */
    for( i=0; i<va->numaudiotracks; i++ ) { /* fill in menu/title audio attributes */
        buf[4+i*8]=(va->ad[i].aformat-1)<<6; /* audio coding mode */
        if( va->ad[i].alangp==AL_LANG ) { /* for title audio, not menu audio */
            buf[4+i*8]|=4; /* language type = as per language code */
            memcpy(buf+6+i*8,va->ad[i].lang,2); /* language code */
        }
	  /* multichannel extension not supported for now */
        if( va->ad[i].adolby==AD_SURROUND ) { /* for title audio, not menu audio */
            buf[4+i*8]|=2; /* application mode = surround */
            buf[11+i*8]=8; /* suitable for Dolby surround decoding */
        }
	  /* karaoke options not supported for now */

        buf[5+i*8]=
            ((va->ad[i].aquant-1)<<6)  | /* quantization/DRC */
            ((va->ad[i].asample-1)<<4) | /* sample rate */
            (va->ad[i].achannels-1); /* nr channels - 1 */

	  /* audio code extension (buf[9 + i * 8], title audio only) currently left unspecified */
    }
    buf[0x55]=va->numsubpicturetracks; /* nr subpicture streams, low byte */
    for( i=0; i<va->numsubpicturetracks; i++ ) {
	  /* coding mode always RLE */
        if( va->sp[i].slangp==AL_LANG ) { /* for title subpicture, not menu subpicture */
            buf[0x56+i*6]=1; /* language type = as per language code */
            memcpy(buf+0x58+i*6,va->sp[i].lang,2); /* language code */
        }
	/* title code extension (buf[0x56 + i * 6 + 5], title subpicture only) currently left unspecified */
    }
}

static int needmenus(const struct menugroup *mg)
/* do I actually have any menu definitions in mg. */
{
    if (!mg ) return 0;
    if( !mg->numgroups ) return 0;
    if( !mg->groups[0].pg->numpgcs ) return 0;
    return 1;
}

static void WriteIFO(FILE *h,const struct workset *ws)
/* writes the IFO for a VTSM. */
{
    static unsigned char buf[2048];
    int i,forcemenus=needmenus(ws->menus);

    // sect 0: VTS toplevel
    memset(buf,0,2048);
    memcpy(buf,"DVDVIDEO-VTS",12);
    buf[33]=0x11; /* version number */
    write4(buf+128,0x7ff);
    i=1;

    write4(buf+0xC8,i); // VTS_PTT_SRPT
    i+=Create_PTT_SRPT(0,ws->titles);

    write4(buf+0xCC,i); // VTS_PGCI
    i+=CreatePGC(0,ws,0);

    if( jumppad || forcemenus ) {
        write4(buf+0xD0,i); // VTSM_PGCI
        i+=CreatePGC(0,ws,1);
    }

    write4(buf+0xD4,i); // VTS_TMAPT
    i+=numsectTMAPT(ws->titles);

    if( jumppad || forcemenus ) {
        write4(buf+0xD8,i); // VTSM_C_ADT
        i+=CreateCallAdr(0,ws->menus->vg);
        
        write4(buf+0xDC,i); // VTSM_VOBU_ADMAP
        i+=numsectVOBUAD(ws->menus->vg);
    }

    write4(buf+0xE0,i); // VTS_C_ADT
    i+=CreateCallAdr(0,ws->titles->vg);

    write4(buf+0xE4,i); // VTS_VOBU_ADMAP
    i+=numsectVOBUAD(ws->titles->vg);

    write4(buf+28,i-1);
    if( jumppad || forcemenus ) {
        write4(buf+0xC0,i);
        i+=getvoblen(ws->menus->vg);
    }
    write4(buf+0xC4,i);
    if( ws->titles->numpgcs )
        i+=getvoblen(ws->titles->vg);
    i+=read4(buf+28);
    write4(buf+12,i);

    if( jumppad || forcemenus )
        BuildAVInfo(buf+256,ws->menus->vg);
    BuildAVInfo(buf+512,ws->titles->vg);
    nfwrite(buf,2048,h);

    // sect 1: VTS_PTT_SRPT
    Create_PTT_SRPT(h,ws->titles);
   
    // sect 2: VTS_PGCIT
    CreatePGC(h,ws,0);

    if( jumppad || forcemenus )
        CreatePGC(h,ws,1);

    // sect 3: ??? VTS_TMAPT
    CreateTMAPT(h,ws->titles);

    if( jumppad || forcemenus ) {
        CreateCallAdr(h,ws->menus->vg);
        CreateVOBUAD(h,ws->menus->vg);
    }
    CreateCallAdr(h,ws->titles->vg);
    CreateVOBUAD(h,ws->titles->vg);
}

void WriteIFOs(char *fbase,const struct workset *ws)
/* writes out a .IFO and corresponding .BUP file. */
{
    FILE *h;
    static char buf[1000];

    if( fbase ) {
        sprintf(buf,"%s_0.IFO",fbase);
        h=fopen(buf,"wb");
        WriteIFO(h,ws);
        fclose(h);
        
        sprintf(buf,"%s_0.BUP",fbase); /* same thing again, backup copy */
        h=fopen(buf,"wb");
        WriteIFO(h,ws);
        fclose(h);
    } else
	  /* dummy write */
        WriteIFO(0,ws);
}

void TocGen(const struct workset *ws,const struct pgc *fpc,char *fname)
/* writes the IFO for a VMGM. */
{
    static unsigned char buf[2048];
    int i,j,vtsstart,forcemenus=needmenus(ws->menus);
    FILE *h;

    h=fopen(fname,"wb");

    memset(buf,0,2048);
    memcpy(buf,"DVDVIDEO-VMG",12);
    buf[0x21]=0x11; /* version number */
    buf[0x27]=1; /* number of volumes */
    buf[0x29]=1; /* volume number */
    buf[0x2a]=1; /* side ID */
    write2(buf+0x3e /* number of title sets */, ws->ts->numvts);
    strncpy((char *)(buf+0x40) /* provider ID */, PACKAGE_STRING, 31);
    buf[0x86]=4; /* start address of FP_PGC = 0x400 */
    i=1;

    write4(buf+0xc4,i); /* sector pointer to TT_SRPT (table of titles) */
    i+=Create_TT_SRPT(0,ws->ts,0); /* just to figure out how many sectors will be needed */

    if( jumppad || forcemenus ) {
        write4(buf+0xc8,i); /* sector pointer to VMGM_PGCI_UT (menu PGC table) */
        i+=CreatePGC(0,ws,2);
    }

    write4(buf+0xd0,i);
	  /* sector pointer to VMG_VTS_ATRT (copies of VTS audio/subpicture atts) */
	  /* I will output it immediately following IFO header */
    i+=(8+ws->ts->numvts*0x30c+2047)/2048; /* round up size of VMG_VTS_ATRT to whole sectors */

    if( jumppad || forcemenus ) {
        write4(buf+0xd8,i);
		  /* sector pointer to VMGM_C_ADT (menu cell address table) */
		  /* I make it follow VMG_VTS_ATRT */
        i+=CreateCallAdr(0,ws->menus->vg); /* how much room it will need */

        write4(buf+0xdc,i); /* sector pointer to VMGM_VOBU_ADMAP (menu VOBU address map) */
        i+=numsectVOBUAD(ws->menus->vg);
    }

    write4(buf+0x1c,i-1); /* last sector of IFO */
    vtsstart=i*2; /* size of two copies including BUP */
    if( jumppad || forcemenus ) {
        write4(buf+0xc0,i); /* start sector of menu VOB */
        vtsstart+=getvoblen(ws->menus->vg);
    }
    write4(buf+0xc,vtsstart-1); /* last sector of VMG set (last sector of BUP) */

    if( forcemenus )
        BuildAVInfo(buf+256,ws->menus->vg);

    // create FPC at 0x400
    buf[0x407]=(getratedenom(ws->menus->vg)==90090?3:1)<<6; // only set frame rate XXX: should check titlesets if there is no VMGM menu
    buf[0x4e5]=0xec; // pointer to command table
    // command table
    buf[0x4ed]=1; // # pre commands
    // commands start at 0x4f4
    i=0x4f4;
    if( fpc ) {
        unsigned char *pi;
        if( fpc->posti || fpc->numsources || fpc->numbuttons || fpc->entries ) {
            fprintf(stderr,"ERR:  FPC can ONLY contain prei commands, nothing else\n");
            exit(1);
        }
        if( ws->menus && ws->menus->numgroups )
            pi=vm_compile(buf+i,buf+i,ws,ws->menus->groups[0].pg,0,fpc->prei,2); // XXX: just use the first pgcgroup as a reference
        else
            pi=vm_compile(buf+i,buf+i,ws,0,0,fpc->prei,2);
        if( !pi ) {
            fprintf(stderr,"ERR:  in FPC\n");
            exit(1);
        }
        i=(pi-buf-i)/8;
        assert(i<=128);
        buf[0x4ed]=i;
    } else if( forcemenus ) {
        buf[i+0]=0x30; // jump to VMGM 1
        buf[i+1]=0x06;
        buf[i+2]=0x00;
        buf[i+3]=0x00;
        buf[i+4]=0x00;
        buf[i+5]=0x42;
        buf[i+6]=0x00;
        buf[i+7]=0x00;
    } else if( ws->ts->numvts && ws->ts->vts[0].hasmenu ) {
        buf[i+0]=0x30; // jump to VTSM vts=1, ttn=1, menu=1
        buf[i+1]=0x06;
        buf[i+2]=0x00;
        buf[i+3]=0x01;
        buf[i+4]=0x01;
        buf[i+5]=0x83;
        buf[i+6]=0x00;
        buf[i+7]=0x00;
    } else {
        buf[i+0]=0x30; // jump to title 1
        buf[i+1]=0x02;
        buf[i+2]=0x00;
        buf[i+3]=0x00;
        buf[i+4]=0x00;
        buf[i+5]=0x01;
        buf[i+6]=0x00;
        buf[i+7]=0x00;
    }
    write2(buf+0x4f2,7+buf[0x4ed]*8);
    write2(buf+0x82 /* end byte address, low word, of VMGI_MAT */,0x4ec+read2(buf+0x4f2));
    nfwrite(buf,2048,h);

    Create_TT_SRPT(h,ws->ts,vtsstart); /* generate it for real */

    // PGC
    if( jumppad || forcemenus )
        CreatePGC(h,ws,2);

    /* VMG_VTS_ATRT contains copies of menu and title attributes from all titlesets */
	/* output immediately following IFO header, as promised above */
    memset(buf,0,2048);
    j=8+ws->ts->numvts*4;
    write2(buf,ws->ts->numvts); /* number of titlesets */
    write4(buf+4,ws->ts->numvts*0x30c+8-1); /* end address (last byte of last VTS_ATRT) */
    for( i=0; i<ws->ts->numvts; i++ )
        write4(buf+8+i*4,j+i*0x308); /* offset to VTS_ATRT i */
    nfwrite(buf,j,h);
    for( i=0; i<ws->ts->numvts; i++ ) /* output each VTS_ATRT */
	  {
        write4(buf,0x307); /* end address */
        memcpy(buf+4,ws->ts->vts[i].vtscat,4);
		  /* VTS_CAT (copy of bytes 0x22 .. 0x25 of VTS IFO) */
        memcpy(buf+8,ws->ts->vts[i].vtssummary,0x300);
		  /* copy of VTS attributes (bytes 0x100 onwards of VTS IFO) */
        nfwrite(buf,0x308,h);
        j+=0x308;
      } /*for*/
    j=2048-(j&2047);
    if( j < 2048 ) { /* pad to next whole sector */
        memset(buf,0,j);
        nfwrite(buf,j,h);
    }

    if( jumppad || forcemenus ) {
        CreateCallAdr(h,ws->menus->vg); /* actually generate VMGM_C_ADT */
        CreateVOBUAD(h,ws->menus->vg); /* generate VMGM_VOBU_ADMAP */
    }
    fclose(h);
}

