/*	$OpenBSD: setup.c,v 1.54 2014/05/29 12:02:50 krw Exp $	*/
/*	$NetBSD: setup.c,v 1.27 1996/09/27 22:45:19 christos Exp $	*/

/*
 * Copyright (c) 1980, 1986, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#define DKTYPENAMES
#include <sys/param.h>
#include <sys/time.h>
#include <ufs/ufs/dinode.h>
#include <ufs/ffs/fs.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/disklabel.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "fsck.h"
#include "extern.h"
#include "fsutil.h"

#define altsblock (*asblk.b_un.b_fs)
#define POWEROF2(num)	(((num) & ((num) - 1)) == 0)

void badsb(int, char *);
int calcsb(char *, int, struct fs *);
static int readsb(int);
static int cmpsb(struct fs *, struct fs *);
static char rdevname[MAXPATHLEN];

long numdirs, listmax, inplast;

/*
 * Possible locations for the superblock.
 */
static const int sbtry[] = SBLOCKSEARCH;
/* locations the 1st alternate sb can be at */
static const int altsbtry[] = { 32, 64, 128, 144, 160, 192, 256 };

int
setup(char *dev)
{
	long cg, size, asked, i, j, bmapsize;
	off_t sizepb;
	struct stat statb;
	struct fs proto;
	int doskipclean;
	int32_t maxsymlinklen, nindir, inopb;
	u_int64_t maxfilesize;
	char *realdev;

	havesb = 0;
	fswritefd = fsreadfd = -1;
	doskipclean = skipclean;
	if ((fsreadfd = opendev(dev, O_RDONLY, 0, &realdev)) < 0) {
		printf("Can't open %s: %s\n", dev, strerror(errno));
		return (0);
	}
	if (strncmp(dev, realdev, PATH_MAX) != 0) {
		blockcheck(unrawname(realdev));
		strlcpy(rdevname, realdev, sizeof(rdevname));
		setcdevname(rdevname, dev, preen);
	}
	if (fstat(fsreadfd, &statb) < 0) {
		printf("Can't stat %s: %s\n", realdev, strerror(errno));
		close(fsreadfd);
		return (0);
	}
	if (!S_ISCHR(statb.st_mode)) {
		pfatal("%s is not a character device", realdev);
		if (reply("CONTINUE") == 0) {
			close(fsreadfd);
			return (0);
		}
	}
	if (preen == 0) {
		printf("** %s", realdev);
		if (strncmp(dev, realdev, PATH_MAX) != 0)
			printf(" (%s)", dev);
	}
	if (nflag || (fswritefd = opendev(dev, O_WRONLY, 0, NULL)) < 0) {
		fswritefd = -1;
		if (preen)
			pfatal("NO WRITE ACCESS");
		printf(" (NO WRITE)");
	}
	if (preen == 0)
		printf("\n");
	fsmodified = 0;
	lfdir = 0;
	initbarea(&sblk);
	initbarea(&asblk);
	sblk.b_un.b_buf = malloc(SBSIZE);
	asblk.b_un.b_buf = malloc(SBSIZE);
	if (sblk.b_un.b_buf == NULL || asblk.b_un.b_buf == NULL)
		errexit("cannot allocate space for superblock\n");
	secsize = DEV_BSIZE;

	/*
	 * Read in the superblock, looking for alternates if necessary
	 */
	if (readsb(1) == 0) {
		if (bflag || preen || calcsb(realdev, fsreadfd, &proto) == 0)
			return(0);
		if (reply("LOOK FOR ALTERNATE SUPERBLOCKS") == 0)
			return (0);
		for (i = 0; i < sizeof(altsbtry) / sizeof(altsbtry[0]); i++) {
			bflag = altsbtry[i];
			/* proto partially setup by calcsb */
			if (readsb(0) != 0 &&
			    proto.fs_fsize == sblock.fs_fsize &&
			    proto.fs_bsize == sblock.fs_bsize)
				goto found;
		}
		for (cg = 0; cg < proto.fs_ncg; cg++) {
			bflag = fsbtodb(&proto, cgsblock(&proto, cg));
			if (readsb(0) != 0)
				break;
		}
		if (cg >= proto.fs_ncg) {
			printf("%s %s\n%s %s\n%s %s\n",
			    "SEARCH FOR ALTERNATE SUPER-BLOCK",
			    "FAILED. YOU MUST USE THE",
			    "-b OPTION TO FSCK_FFS TO SPECIFY THE",
			    "LOCATION OF AN ALTERNATE",
			    "SUPER-BLOCK TO SUPPLY NEEDED",
			    "INFORMATION; SEE fsck_ffs(8).");
			return(0);
		}
found:
		doskipclean = 0;
		pwarn("USING ALTERNATE SUPERBLOCK AT %d\n", bflag);
	}
	if (debug)
		printf("clean = %d\n", sblock.fs_clean);
	if (sblock.fs_clean & FS_ISCLEAN) {
		if (doskipclean) {
			pwarn("%sile system is clean; not checking\n",
			    preen ? "f" : "** F");
			return (-1);
		}
		if (!preen)
			pwarn("** File system is already clean\n");
	}
	maxfsblock = sblock.fs_size;
	maxino = sblock.fs_ncg * sblock.fs_ipg;
	sizepb = sblock.fs_bsize;
	maxfilesize = sblock.fs_bsize * NDADDR - 1;
	for (i = 0; i < NIADDR; i++) {
		sizepb *= NINDIR(&sblock);
		maxfilesize += sizepb;
	}
	/*
	 * Check and potentially fix certain fields in the super block.
	 */
	if (sblock.fs_optim != FS_OPTTIME && sblock.fs_optim != FS_OPTSPACE) {
		pfatal("UNDEFINED OPTIMIZATION IN SUPERBLOCK");
		if (reply("SET TO DEFAULT") == 1) {
			sblock.fs_optim = FS_OPTTIME;
			sbdirty();
		}
	}
	if ((sblock.fs_minfree < 0 || sblock.fs_minfree > 99)) {
		pfatal("IMPOSSIBLE MINFREE=%d IN SUPERBLOCK",
		    sblock.fs_minfree);
		if (reply("SET TO DEFAULT") == 1) {
			sblock.fs_minfree = 10;
			sbdirty();
		}
	}
	if (sblock.fs_npsect < sblock.fs_nsect ||
	    sblock.fs_npsect > sblock.fs_nsect*2) {
		pwarn("IMPOSSIBLE NPSECT=%d IN SUPERBLOCK",
		    sblock.fs_npsect);
		sblock.fs_npsect = sblock.fs_nsect;
		if (preen)
			printf(" (FIXED)\n");
		if (preen || reply("SET TO DEFAULT") == 1) {
			sbdirty();
			dirty(&asblk);
		}
	}
	if (sblock.fs_bmask != ~(sblock.fs_bsize - 1)) {
		pwarn("INCORRECT BMASK=%x IN SUPERBLOCK",
		    sblock.fs_bmask);
		sblock.fs_bmask = ~(sblock.fs_bsize - 1);
		if (preen)
			printf(" (FIXED)\n");
		if (preen || reply("FIX") == 1) {
			sbdirty();
			dirty(&asblk);
		}
	}
	if (sblock.fs_fmask != ~(sblock.fs_fsize - 1)) {
		pwarn("INCORRECT FMASK=%x IN SUPERBLOCK",
		    sblock.fs_fmask);
		sblock.fs_fmask = ~(sblock.fs_fsize - 1);
		if (preen)
			printf(" (FIXED)\n");
		if (preen || reply("FIX") == 1) {
			sbdirty();
			dirty(&asblk);
		}
	}
	if (1 << sblock.fs_bshift != sblock.fs_bsize) {
		pwarn("INCORRECT BSHIFT=%d IN SUPERBLOCK", sblock.fs_bshift);
		sblock.fs_bshift = ffs(sblock.fs_bsize) - 1;
		if (preen)
			printf(" (FIXED)\n");
		if (preen || reply("FIX") == 1) {
			sbdirty();
			dirty(&asblk);
		}
	}
	if (1 << sblock.fs_fshift != sblock.fs_fsize) {
		pwarn("INCORRECT FSHIFT=%d IN SUPERBLOCK", sblock.fs_fshift);
		sblock.fs_fshift = ffs(sblock.fs_fsize) - 1;
		if (preen)
			printf(" (FIXED)\n");
		if (preen || reply("FIX") == 1) {
			sbdirty();
			dirty(&asblk);
		}
	}
	if (sblock.fs_inodefmt < FS_44INODEFMT) {
		pwarn("Format of filesystem is too old.\n");
		pwarn("Must update to modern format using a version of fsck\n");
		pfatal("from before release 5.0 with the command ``fsck -c 2''\n");
		exit(8);
	}
	if (sblock.fs_maxfilesize != maxfilesize) {
		pwarn("INCORRECT MAXFILESIZE=%u IN SUPERBLOCK",
		    sblock.fs_maxfilesize);
		sblock.fs_maxfilesize = maxfilesize;
		if (preen)
			printf(" (FIXED)\n");
		if (preen || reply("FIX") == 1) {
			sbdirty();
			dirty(&asblk);
		}
	}
	maxsymlinklen = MAXSYMLINKLEN;
	if (sblock.fs_maxsymlinklen != maxsymlinklen) {
		pwarn("INCORRECT MAXSYMLINKLEN=%d IN SUPERBLOCK",
		    sblock.fs_maxsymlinklen);
		sblock.fs_maxsymlinklen = maxsymlinklen;
		if (preen)
			printf(" (FIXED)\n");
		if (preen || reply("FIX") == 1) {
			sbdirty();
			dirty(&asblk);
		}
	}
	if (sblock.fs_qbmask != ~sblock.fs_bmask) {
		pwarn("INCORRECT QBMASK=%lx IN SUPERBLOCK",
		    (unsigned long)sblock.fs_qbmask);
		sblock.fs_qbmask = ~sblock.fs_bmask;
		if (preen)
			printf(" (FIXED)\n");
		if (preen || reply("FIX") == 1) {
			sbdirty();
			dirty(&asblk);
		}
	}
	if (sblock.fs_qfmask != ~sblock.fs_fmask) {
		pwarn("INCORRECT QFMASK=%lx IN SUPERBLOCK",
		    (unsigned long)sblock.fs_qfmask);
		sblock.fs_qfmask = ~sblock.fs_fmask;
		if (preen)
			printf(" (FIXED)\n");
		if (preen || reply("FIX") == 1) {
			sbdirty();
			dirty(&asblk);
		}
	}
	if (sblock.fs_cgsize != fragroundup(&sblock, CGSIZE(&sblock))) {
		pwarn("INCONSISTENT CGSIZE=%d\n", sblock.fs_cgsize);
		sblock.fs_cgsize = fragroundup(&sblock, CGSIZE(&sblock));
		if (preen)
			printf(" (FIXED)\n");
		if (preen || reply("FIX") == 1) {
			sbdirty();
			dirty(&asblk);
		}
	}
	inopb = sblock.fs_bsize / sizeof(struct dinode);
	if (INOPB(&sblock) != inopb) {
		pwarn("INCONSISTENT INOPB=%d\n", INOPB(&sblock));
		sblock.fs_inopb = inopb;
		if (preen)
			printf(" (FIXED)\n");
		if (preen || reply("FIX") == 1) {
			sbdirty();
			dirty(&asblk);
		}
	}
	nindir = sblock.fs_bsize / sizeof(int32_t);
	if (NINDIR(&sblock) != nindir) {
		pwarn("INCONSISTENT NINDIR=%d\n", NINDIR(&sblock));
		sblock.fs_nindir = nindir;
		if (preen)
			printf(" (FIXED)\n");
		if (preen || reply("FIX") == 1) {
			sbdirty();
			dirty(&asblk);
		}
	}
	if (asblk.b_dirty && !bflag) {
		memcpy(&altsblock, &sblock, (size_t)sblock.fs_sbsize);
		flush(fswritefd, &asblk);
	}
	/*
	 * read in the summary info.
	 */
	asked = 0;
	sblock.fs_csp = calloc(1, sblock.fs_cssize);
	if (sblock.fs_csp == NULL) {
		printf("cannot alloc %u bytes for cylinder group summary area\n",
		    (unsigned)sblock.fs_cssize);
		goto badsblabel;
	}
	for (i = 0, j = 0; i < sblock.fs_cssize; i += sblock.fs_bsize, j++) {
		size = sblock.fs_cssize - i < sblock.fs_bsize ?
		    sblock.fs_cssize - i : sblock.fs_bsize;
		if (bread(fsreadfd, (char *)sblock.fs_csp + i,
		    fsbtodb(&sblock, sblock.fs_csaddr + j * sblock.fs_frag),
		    size) != 0 && !asked) {
			pfatal("BAD SUMMARY INFORMATION");
			if (reply("CONTINUE") == 0) {
				ckfini(0);
				errexit("%s", "");
			}
			asked++;
		}
	}
	/*
	 * allocate and initialize the necessary maps
	 */
	bmapsize = roundup(howmany(maxfsblock, NBBY), sizeof(int16_t));
	blockmap = calloc((unsigned)bmapsize, sizeof(char));
	if (blockmap == NULL) {
		printf("cannot alloc %u bytes for blockmap\n",
		    (unsigned)bmapsize);
		goto badsblabel;
	}
	inostathead = calloc((unsigned)(sblock.fs_ncg),
	    sizeof(struct inostatlist));
	if (inostathead == NULL) {
		printf("cannot alloc %u bytes for inostathead\n",
		    (unsigned)(sizeof(struct inostatlist) * (sblock.fs_ncg)));
		goto badsblabel;
	}
	numdirs = MAX(sblock.fs_cstotal.cs_ndir, 128);
	inplast = 0;
	listmax = numdirs + 10;
	inpsort = calloc((unsigned)listmax, sizeof(struct inoinfo *));
	if (inpsort == NULL) {
		printf("cannot alloc %zu bytes for inpsort\n",
		    (unsigned)listmax * sizeof(struct inoinfo *));
		goto badsblabel;
	}
	inphead = calloc((unsigned)numdirs, sizeof(struct inoinfo *));
	if (inphead == NULL) {
		printf("cannot alloc %zu bytes for inphead\n",
		    (unsigned)numdirs * sizeof(struct inoinfo *));
		goto badsblabel;
	}
	bufinit();
	if (sblock.fs_flags & FS_DOSOFTDEP)
		usedsoftdep = 1;
	else
		usedsoftdep = 0;
	return (1);

badsblabel:
	ckfini(0);
	return (0);
}


/*
 * Read in the super block and its summary info.
 */
static int
readsb(int listerr)
{
	daddr_t super = 0;
	int i;

	if (bflag) {
		super = bflag;

		if (bread(fsreadfd, (char *)&sblock, super, (long)SBSIZE) != 0)
			return (0);

		if (sblock.fs_magic != FS_MAGIC) {
			badsb(listerr, "MAGIC NUMBER WRONG");
			return (0);
		}
	} else {
		for (i = 0; sbtry[i] != -1; i++) {
			super = sbtry[i] / DEV_BSIZE;

			if (bread(fsreadfd, (char *)&sblock, super,
			    (long)SBSIZE) != 0)
				return (0);

			if (sblock.fs_magic != FS_MAGIC)
				continue; /* Not a superblock */
			break;
		}

		if (sbtry[i] == -1) {
			badsb(listerr, "MAGIC NUMBER WRONG");
			return (0);
		}
	}

	sblk.b_bno = super;
	sblk.b_size = SBSIZE;

	/*
	 * run a few consistency checks of the super block
	 */
	if (sblock.fs_ncg < 1) {
		badsb(listerr, "NCG OUT OF RANGE");
		return (0);
	}
	if (sblock.fs_cpg < 1) {
		badsb(listerr, "CPG OUT OF RANGE");
		return (0);
	}
	if (sblock.fs_magic == FS_MAGIC) {
		if (sblock.fs_ncg * sblock.fs_cpg < sblock.fs_ncyl ||
		    (sblock.fs_ncg - 1) * sblock.fs_cpg >= sblock.fs_ncyl) {
			badsb(listerr, "NCYL LESS THAN NCG*CPG");
			return (0);
		}
	}
	if (sblock.fs_sbsize > SBSIZE) {
		badsb(listerr, "SBSIZE PREPOSTEROUSLY LARGE");
		return (0);
	}

	if (!POWEROF2(sblock.fs_bsize) || sblock.fs_bsize < MINBSIZE ||
	    sblock.fs_bsize > MAXBSIZE) {
		badsb(listerr, "ILLEGAL BLOCK SIZE IN SUPERBLOCK");
		return (0);
	}

	if (!POWEROF2(sblock.fs_fsize) || sblock.fs_fsize > sblock.fs_bsize ||
	    sblock.fs_fsize < sblock.fs_bsize / MAXFRAG) {
		badsb(listerr, "ILLEGAL FRAGMENT SIZE IN SUPERBLOCK");
		return (0);
	}

	if (bflag)
		goto out;
	getblk(&asblk, cgsblock(&sblock, sblock.fs_ncg - 1), sblock.fs_sbsize);
	if (asblk.b_errs)
		return (0);
	if (cmpsb(&sblock, &altsblock)) {
		if (debug) {
			long *nlp, *olp, *endlp;

			printf("superblock mismatches\n");
			nlp = (long *)&altsblock;
			olp = (long *)&sblock;
			endlp = olp + (sblock.fs_sbsize / sizeof *olp);
			for ( ; olp < endlp; olp++, nlp++) {
				if (*olp == *nlp)
					continue;
				printf("offset %d, original %ld, alternate %ld\n",
				    (int)(olp - (long *)&sblock), *olp, *nlp);
			}
		}
		badsb(listerr,
		    "VALUES IN SUPER BLOCK DISAGREE WITH THOSE IN LAST ALTERNATE");
		return (0);
	}
out:
	havesb = 1;
	return (1);
}

void
badsb(int listerr, char *s)
{

	if (!listerr)
		return;
	if (preen)
		printf("%s: ", cdevname());
	pfatal("BAD SUPER BLOCK: %s\n", s);
}

static struct diskpart *
getdiskpart(s, fd)
	char *s;
	int fd;
{
	static struct diskpart lab;

	if (ioctl(fd, DIOCGETPART, (char *)&lab) < 0) {
		if (s == NULL)
			return 0;
		pwarn("ioctl (DIOCGETPART): %s\n", strerror(errno));
		errexit("%s: can't read disk label", s);
	}
	return &lab;
}

/*
 * Calculate a prototype superblock based on information in the disk label.
 * When done the cgsblock macro can be calculated and the fs_ncg field
 * can be used. Do NOT attempt to use other macros without verifying that
 * their needed information is available!
 */
int
calcsb(char *dev, int devfd, struct fs *fs)
{
	register struct diskpart *pp;
	char *cp;
	int i;

	cp = strchr(dev, '\0');
	if ((cp == NULL || (cp[-1] < 'a' || cp[-1] >= 'a' + MAXPARTITIONS)) &&
	    !isdigit((unsigned char)cp[-1])) {
		pfatal("%s: CANNOT FIGURE OUT FILE SYSTEM PARTITION\n", dev);
		return (0);
	}
	pp = getdiskpart(dev, devfd);
	if (pp->dp_type != PTYPE_BSDFFS) {
		pfatal("%s: PARTITION IS NOT A BSD FILE SYSTEM (%02x)\n",
			dev, pp->dp_type);
		return (0);
	}
	memset(fs, 0, sizeof(struct fs));
	fs->fs_fsize = 4096;
	fs->fs_frag = 1;
	fs->fs_bsize = fs->fs_fsize * fs->fs_frag;
	fs->fs_cpg = 100;               /* 50Mb per group */
	fs->fs_nspf = fs->fs_fsize / DEV_BSIZE;
	/*
	 * fs->fs_size is in fragments, DL_GETPSIZE() is in disk sectors
	 * and fs_nspf is in DEV_BSIZE blocks. Shake well.
	 */
	fs->fs_size = pp->dp_size / fs->fs_nspf;
	fs->fs_ntrak = 32;
	fs->fs_nsect = 32;
	fs->fs_spc = 1024;
	fs->fs_sblkno = roundup(
		howmany(SBLOCK + SBSIZE, fs->fs_fsize),
		fs->fs_frag);
again:
	fs->fs_cgmask = 0xffffffff;
	for (i = fs->fs_ntrak; i > 1; i >>= 1)
		fs->fs_cgmask <<= 1;
	if (!POWEROF2(fs->fs_ntrak))
		fs->fs_cgmask <<= 1;
	fs->fs_cgoffset = roundup(
		howmany(fs->fs_nsect, NSPF(fs)), fs->fs_frag);
	fs->fs_fpg = (fs->fs_cpg * fs->fs_spc) / NSPF(fs);
	fs->fs_ncg = howmany(pp->dp_size / fs->fs_spc, fs->fs_cpg);
	for (fs->fs_fsbtodb = 0, i = NSPF(fs); i > 1; i >>= 1)
		fs->fs_fsbtodb++;
	/*
	 * Mimic what mkfs is doing to get an acceptable cgsize,
	 * not all fields used by CGSIZE() are filled in, but it's a best
	 * effort anyway.
	 */
	if (CGSIZE(fs) > fs->fs_bsize && fs->fs_ntrak > 1) {
		fs->fs_ntrak >>= 1;
		fs->fs_spc >>= 1;
		goto again;
	}
	return (1);
}

/*
 * Compare two superblocks
 */
static int
cmpsb(struct fs *sb, struct fs *asb)
{
	/*
	 * Only compare fields which should be the same, and ignore ones
	 * likely to change to ensure future compatibility.
	 */
	if (asb->fs_sblkno != sb->fs_sblkno ||
	    asb->fs_cblkno != sb->fs_cblkno ||
	    asb->fs_iblkno != sb->fs_iblkno ||
	    asb->fs_dblkno != sb->fs_dblkno ||
	    asb->fs_cgoffset != sb->fs_cgoffset ||
	    asb->fs_cgmask != sb->fs_cgmask ||
	    asb->fs_ncg != sb->fs_ncg ||
	    asb->fs_bsize != sb->fs_bsize ||
	    asb->fs_fsize != sb->fs_fsize ||
	    asb->fs_frag != sb->fs_frag ||
	    asb->fs_bmask != sb->fs_bmask ||
	    asb->fs_fmask != sb->fs_fmask ||
	    asb->fs_bshift != sb->fs_bshift ||
	    asb->fs_fshift != sb->fs_fshift ||
	    asb->fs_fragshift != sb->fs_fragshift ||
	    asb->fs_fsbtodb != sb->fs_fsbtodb ||
	    asb->fs_sbsize != sb->fs_sbsize ||
	    asb->fs_nindir != sb->fs_nindir ||
	    asb->fs_inopb != sb->fs_inopb ||
	    asb->fs_cssize != sb->fs_cssize ||
	    asb->fs_cpg != sb->fs_cpg ||
	    asb->fs_ipg != sb->fs_ipg ||
	    asb->fs_fpg != sb->fs_fpg ||
	    asb->fs_magic != sb->fs_magic)
		    return (1);
	/* they're the same */
	return (0);
}
