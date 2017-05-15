#ifndef lint
static const char RCSid[] = "$Id: m_bsdf.c,v 2.34 2017/05/15 22:50:33 greg Exp $";
#endif
/*
 *  Shading for materials with BSDFs taken from XML data files
 */

#include "copyright.h"

#include  "ray.h"
#include  "ambient.h"
#include  "source.h"
#include  "func.h"
#include  "bsdf.h"
#include  "random.h"
#include  "pmapmat.h"

/*
 *	Arguments to this material include optional diffuse colors.
 *  String arguments include the BSDF and function files.
 *	A non-zero thickness causes the strange but useful behavior
 *  of translating transmitted rays this distance beneath the surface
 *  (opposite the surface normal) to bypass any intervening geometry.
 *  Translation only affects scattered, non-source-directed samples.
 *  A non-zero thickness has the further side-effect that an unscattered
 *  (view) ray will pass right through our material if it has any
 *  non-diffuse transmission, making the BSDF surface invisible.  This
 *  shows the proxied geometry instead. Thickness has the further
 *  effect of turning off reflection on the hidden side so that rays
 *  heading in the opposite direction pass unimpeded through the BSDF
 *  surface.  A paired surface may be placed on the opposide side of
 *  the detail geometry, less than this thickness away, if a two-way
 *  proxy is desired.  Note that the sign of the thickness is important.
 *  A positive thickness hides geometry behind the BSDF surface and uses
 *  front reflectance and transmission properties.  A negative thickness
 *  hides geometry in front of the surface when rays hit from behind,
 *  and applies only the transmission and backside reflectance properties.
 *  Reflection is ignored on the hidden side, as those rays pass through.
 *	The "up" vector for the BSDF is given by three variables, defined
 *  (along with the thickness) by the named function file, or '.' if none.
 *  Together with the surface normal, this defines the local coordinate
 *  system for the BSDF.
 *	We do not reorient the surface, so if the BSDF has no back-side
 *  reflectance and none is given in the real arguments, a BSDF surface
 *  with zero thickness will appear black when viewed from behind
 *  unless backface visibility is off.
 *	The diffuse arguments are added to components in the BSDF file,
 *  not multiplied.  However, patterns affect this material as a multiplier
 *  on everything except non-diffuse reflection.
 *
 *  Arguments for MAT_BSDF are:
 *	6+	thick	BSDFfile	ux uy uz	funcfile	transform
 *	0
 *	0|3|6|9	rdf	gdf	bdf
 *		rdb	gdb	bdb
 *		rdt	gdt	bdt
 */

/*
 * Note that our reverse ray-tracing process means that the positions
 * of incoming and outgoing vectors may be reversed in our calls
 * to the BSDF library.  This is fine, since the bidirectional nature
 * of the BSDF (that's what the 'B' stands for) means it all works out.
 */

typedef struct {
	OBJREC	*mp;		/* material pointer */
	RAY	*pr;		/* intersected ray */
	FVECT	pnorm;		/* perturbed surface normal */
	FVECT	vray;		/* local outgoing (return) vector */
	double	sr_vpsa[2];	/* sqrt of BSDF projected solid angle extrema */
	RREAL	toloc[3][3];	/* world to local BSDF coords */
	RREAL	fromloc[3][3];	/* local BSDF coords to world */
	double	thick;		/* surface thickness */
	COLOR	cthru;		/* through component multiplier */
	SDData	*sd;		/* loaded BSDF data */
	COLOR	rdiff;		/* diffuse reflection */
	COLOR	tdiff;		/* diffuse transmission */
}  BSDFDAT;		/* BSDF material data */

#define	cvt_sdcolor(cv, svp)	ccy2rgb(&(svp)->spec, (svp)->cieY, cv)

/* Compute through component color */
static void
compute_through(BSDFDAT *ndp)
{
#define NDIR2CHECK	13
	static const float	dir2check[NDIR2CHECK][2] = {
					{0, 0},
					{-0.8, 0},
					{0, 0.8},
					{0, -0.8},
					{0.8, 0},
					{-0.8, 0.8},
					{-0.8, -0.8},
					{0.8, 0.8},
					{0.8, -0.8},
					{-1.6, 0},
					{0, 1.6},
					{0, -1.6},
					{1.6, 0},
				};
	const double	peak_over = 2.0;
	SDSpectralDF	*dfp;
	FVECT		pdir;
	double		tomega, srchrad;
	COLOR		vpeak, vsum;
	int		nsum, i;
	SDError		ec;

	setcolor(ndp->cthru, .0, .0, .0);	/* starting assumption */

	if (ndp->pr->rod > 0)
		dfp = (ndp->sd->tf != NULL) ? ndp->sd->tf : ndp->sd->tb;
	else
		dfp = (ndp->sd->tb != NULL) ? ndp->sd->tb : ndp->sd->tf;

	if (dfp == NULL)
		return;				/* no specular transmission */
	if (bright(ndp->pr->pcol) <= FTINY)
		return;				/* pattern is black, here */
	srchrad = sqrt(dfp->minProjSA);		/* else search for peak */
	setcolor(vpeak, .0, .0, .0);
	setcolor(vsum, .0, .0, .0);
	nsum = 0;
	for (i = 0; i < NDIR2CHECK; i++) {
		FVECT	tdir;
		SDValue	sv;
		COLOR	vcol;
		tdir[0] = -ndp->vray[0] + dir2check[i][0]*srchrad;
		tdir[1] = -ndp->vray[1] + dir2check[i][1]*srchrad;
		tdir[2] = -ndp->vray[2];
		if (normalize(tdir) == 0)
			continue;
		ec = SDevalBSDF(&sv, tdir, ndp->vray, ndp->sd);
		if (ec)
			goto baderror;
		cvt_sdcolor(vcol, &sv);
		addcolor(vsum, vcol);
		++nsum;
		if (bright(vcol) > bright(vpeak)) {
			copycolor(vpeak, vcol);
			VCOPY(pdir, tdir);
		}
	}
	ec = SDsizeBSDF(&tomega, pdir, ndp->vray, SDqueryMin, ndp->sd);
	if (ec)
		goto baderror;
	if (tomega > 1.5*dfp->minProjSA)
		return;				/* not really a peak? */
	if ((bright(vpeak) - ndp->sd->tLamb.cieY*(1./PI))*tomega <= .001)
		return;				/* < 0.1% transmission */
	for (i = 3; i--; )			/* remove peak from average */
		colval(vsum,i) -= colval(vpeak,i);
	--nsum;
	if (peak_over*bright(vsum) >= nsum*bright(vpeak))
		return;				/* not peaky enough */
	copycolor(ndp->cthru, vpeak);		/* else use it */
	scalecolor(ndp->cthru, tomega);
	multcolor(ndp->cthru, ndp->pr->pcol);	/* modify by pattern */
	return;
baderror:
	objerror(ndp->mp, USER, transSDError(ec));
#undef NDIR2CHECK
}

/* Jitter ray sample according to projected solid angle and specjitter */
static void
bsdf_jitter(FVECT vres, BSDFDAT *ndp, double sr_psa)
{
	VCOPY(vres, ndp->vray);
	if (specjitter < 1.)
		sr_psa *= specjitter;
	if (sr_psa <= FTINY)
		return;
	vres[0] += sr_psa*(.5 - frandom());
	vres[1] += sr_psa*(.5 - frandom());
	normalize(vres);
}

/* Get BSDF specular for direct component, returning true if OK to proceed */
static int
direct_specular_OK(COLOR cval, FVECT ldir, double omega, BSDFDAT *ndp)
{
	int	nsamp, ok = 0;
	FVECT	vsrc, vsmp, vjit;
	double	tomega;
	double	sf, tsr, sd[2];
	COLOR	csmp, cdiff;
	double	diffY;
	SDValue	sv;
	SDError	ec;
	int	i;
					/* transform source direction */
	if (SDmapDir(vsrc, ndp->toloc, ldir) != SDEnone)
		return(0);
					/* will discount diffuse portion */
	switch ((vsrc[2] > 0)<<1 | (ndp->vray[2] > 0)) {
	case 3:
		if (ndp->sd->rf == NULL)
			return(0);	/* all diffuse */
		sv = ndp->sd->rLambFront;
		break;
	case 0:
		if (ndp->sd->rb == NULL)
			return(0);	/* all diffuse */
		sv = ndp->sd->rLambBack;
		break;
	default:
		if ((ndp->sd->tf == NULL) & (ndp->sd->tb == NULL))
			return(0);	/* all diffuse */
		sv = ndp->sd->tLamb;
		break;
	}
	if (sv.cieY > FTINY) {
		diffY = sv.cieY *= 1./PI;
		cvt_sdcolor(cdiff, &sv);
	} else {
		diffY = .0;
		setcolor(cdiff, .0, .0, .0);
	}
					/* assign number of samples */
	ec = SDsizeBSDF(&tomega, ndp->vray, vsrc, SDqueryMin, ndp->sd);
	if (ec)
		goto baderror;
					/* check indirect over-counting */
	if ((ndp->thick != 0 || bright(ndp->cthru) > FTINY)
				&& ndp->pr->crtype & (SPECULAR|AMBIENT)
				&& (vsrc[2] > 0) ^ (ndp->vray[2] > 0)) {
		double	dx = vsrc[0] + ndp->vray[0];
		double	dy = vsrc[1] + ndp->vray[1];
		if (dx*dx + dy*dy <= omega+tomega)
			return(0);
	}
	sf = specjitter * ndp->pr->rweight;
	if (tomega <= .0)
		nsamp = 1;
	else if (25.*tomega <= omega)
		nsamp = 100.*sf + .5;
	else
		nsamp = 4.*sf*omega/tomega + .5;
	nsamp += !nsamp;
	setcolor(cval, .0, .0, .0);	/* sample our source area */
	sf = sqrt(omega);
	tsr = sqrt(tomega);
	for (i = nsamp; i--; ) {
		VCOPY(vsmp, vsrc);	/* jitter query directions */
		if (nsamp > 1) {
			multisamp(sd, 2, (i + frandom())/(double)nsamp);
			vsmp[0] += (sd[0] - .5)*sf;
			vsmp[1] += (sd[1] - .5)*sf;
			if (normalize(vsmp) == 0) {
				--nsamp;
				continue;
			}
		}
		bsdf_jitter(vjit, ndp, tsr);
					/* compute BSDF */
		ec = SDevalBSDF(&sv, vjit, vsmp, ndp->sd);
		if (ec)
			goto baderror;
		if (sv.cieY - diffY <= FTINY) {
			addcolor(cval, cdiff);
			continue;	/* no specular part */
		}
		cvt_sdcolor(csmp, &sv);
		addcolor(cval, csmp);	/* else average it in */
		++ok;
	}
	if (!ok) {
		setcolor(cval, .0, .0, .0);
		return(0);		/* no valid specular samples */
	}
	sf = 1./(double)nsamp;
	scalecolor(cval, sf);
					/* subtract diffuse contribution */
	for (i = 3*(diffY > FTINY); i--; )
		if ((colval(cval,i) -= colval(cdiff,i)) < .0)
			colval(cval,i) = .0;
	return(1);
baderror:
	objerror(ndp->mp, USER, transSDError(ec));
	return(0);			/* gratis return */
}

/* Compute source contribution for BSDF (reflected & transmitted) */
static void
dir_bsdf(
	COLOR  cval,			/* returned coefficient */
	void  *nnp,			/* material data */
	FVECT  ldir,			/* light source direction */
	double  omega			/* light source size */
)
{
	BSDFDAT		*np = (BSDFDAT *)nnp;
	double		ldot;
	double		dtmp;
	COLOR		ctmp;

	setcolor(cval, .0, .0, .0);

	ldot = DOT(np->pnorm, ldir);
	if ((-FTINY <= ldot) & (ldot <= FTINY))
		return;

	if (ldot > 0 && bright(np->rdiff) > FTINY) {
		/*
		 *  Compute added diffuse reflected component.
		 */
		copycolor(ctmp, np->rdiff);
		dtmp = ldot * omega * (1./PI);
		scalecolor(ctmp, dtmp);
		addcolor(cval, ctmp);
	}
	if (ldot < 0 && bright(np->tdiff) > FTINY) {
		/*
		 *  Compute added diffuse transmission.
		 */
		copycolor(ctmp, np->tdiff);
		dtmp = -ldot * omega * (1.0/PI);
		scalecolor(ctmp, dtmp);
		addcolor(cval, ctmp);
	}
	if (ambRayInPmap(np->pr))
		return;		/* specular already in photon map */
	/*
	 *  Compute specular scattering coefficient using BSDF.
	 */
	if (!direct_specular_OK(ctmp, ldir, omega, np))
		return;
	if (ldot < 0) {		/* pattern for specular transmission */
		multcolor(ctmp, np->pr->pcol);
		dtmp = -ldot * omega;
	} else
		dtmp = ldot * omega;
	scalecolor(ctmp, dtmp);
	addcolor(cval, ctmp);
}

/* Compute source contribution for BSDF (reflected only) */
static void
dir_brdf(
	COLOR  cval,			/* returned coefficient */
	void  *nnp,			/* material data */
	FVECT  ldir,			/* light source direction */
	double  omega			/* light source size */
)
{
	BSDFDAT		*np = (BSDFDAT *)nnp;
	double		ldot;
	double		dtmp;
	COLOR		ctmp, ctmp1, ctmp2;

	setcolor(cval, .0, .0, .0);

	ldot = DOT(np->pnorm, ldir);
	
	if (ldot <= FTINY)
		return;

	if (bright(np->rdiff) > FTINY) {
		/*
		 *  Compute added diffuse reflected component.
		 */
		copycolor(ctmp, np->rdiff);
		dtmp = ldot * omega * (1./PI);
		scalecolor(ctmp, dtmp);
		addcolor(cval, ctmp);
	}
	if (ambRayInPmap(np->pr))
		return;		/* specular already in photon map */
	/*
	 *  Compute specular reflection coefficient using BSDF.
	 */
	if (!direct_specular_OK(ctmp, ldir, omega, np))
		return;
	dtmp = ldot * omega;
	scalecolor(ctmp, dtmp);
	addcolor(cval, ctmp);
}

/* Compute source contribution for BSDF (transmitted only) */
static void
dir_btdf(
	COLOR  cval,			/* returned coefficient */
	void  *nnp,			/* material data */
	FVECT  ldir,			/* light source direction */
	double  omega			/* light source size */
)
{
	BSDFDAT		*np = (BSDFDAT *)nnp;
	double		ldot;
	double		dtmp;
	COLOR		ctmp;

	setcolor(cval, .0, .0, .0);

	ldot = DOT(np->pnorm, ldir);

	if (ldot >= -FTINY)
		return;

	if (bright(np->tdiff) > FTINY) {
		/*
		 *  Compute added diffuse transmission.
		 */
		copycolor(ctmp, np->tdiff);
		dtmp = -ldot * omega * (1.0/PI);
		scalecolor(ctmp, dtmp);
		addcolor(cval, ctmp);
	}
	if (ambRayInPmap(np->pr))
		return;		/* specular already in photon map */
	/*
	 *  Compute specular scattering coefficient using BSDF.
	 */
	if (!direct_specular_OK(ctmp, ldir, omega, np))
		return;
					/* full pattern on transmission */
	multcolor(ctmp, np->pr->pcol);
	dtmp = -ldot * omega;
	scalecolor(ctmp, dtmp);
	addcolor(cval, ctmp);
}

/* Sample separate BSDF component */
static int
sample_sdcomp(BSDFDAT *ndp, SDComponent *dcp, int usepat)
{
	int	nstarget = 1;
	int	nsent;
	SDError	ec;
	SDValue bsv;
	double	xrand;
	FVECT	vsmp;
	RAY	sr;
						/* multiple samples? */
	if (specjitter > 1.5) {
		nstarget = specjitter*ndp->pr->rweight + .5;
		nstarget += !nstarget;
	}
						/* run through our samples */
	for (nsent = 0; nsent < nstarget; nsent++) {
		if (nstarget == 1) {		/* stratify random variable */
			xrand = urand(ilhash(dimlist,ndims)+samplendx);
			if (specjitter < 1.)
				xrand = .5 + specjitter*(xrand-.5);
		} else {
			xrand = (nsent + frandom())/(double)nstarget;
		}
		SDerrorDetail[0] = '\0';	/* sample direction & coef. */
		bsdf_jitter(vsmp, ndp, ndp->sr_vpsa[0]);
		ec = SDsampComponent(&bsv, vsmp, xrand, dcp);
		if (ec)
			objerror(ndp->mp, USER, transSDError(ec));
		if (bsv.cieY <= FTINY)		/* zero component? */
			break;
						/* map vector to world */
		if (SDmapDir(sr.rdir, ndp->fromloc, vsmp) != SDEnone)
			break;
						/* spawn a specular ray */
		if (nstarget > 1)
			bsv.cieY /= (double)nstarget;
		cvt_sdcolor(sr.rcoef, &bsv);	/* use sample color */
		if (usepat)			/* apply pattern? */
			multcolor(sr.rcoef, ndp->pr->pcol);
		if (rayorigin(&sr, SPECULAR, ndp->pr, sr.rcoef) < 0) {
			if (maxdepth > 0)
				break;
			continue;		/* Russian roulette victim */
		}
						/* need to offset origin? */
		if (ndp->thick != 0 && (ndp->pr->rod > 0) ^ (vsmp[2] > 0))
			VSUM(sr.rorg, sr.rorg, ndp->pr->ron, -ndp->thick);
		rayvalue(&sr);			/* send & evaluate sample */
		multcolor(sr.rcol, sr.rcoef);
		addcolor(ndp->pr->rcol, sr.rcol);
	}
	return(nsent);
}

/* Sample non-diffuse components of BSDF */
static int
sample_sdf(BSDFDAT *ndp, int sflags)
{
	int		n, ntotal = 0;
	SDSpectralDF	*dfp;
	COLORV		*unsc;

	if (sflags == SDsampSpT) {
		unsc = ndp->tdiff;
		if (ndp->pr->rod > 0)
			dfp = (ndp->sd->tf != NULL) ? ndp->sd->tf : ndp->sd->tb;
		else
			dfp = (ndp->sd->tb != NULL) ? ndp->sd->tb : ndp->sd->tf;
	} else /* sflags == SDsampSpR */ {
		unsc = ndp->rdiff;
		if (ndp->pr->rod > 0)
			dfp = ndp->sd->rf;
		else
			dfp = ndp->sd->rb;
	}
	if (dfp == NULL)			/* no specular component? */
		return(0);
						/* below sampling threshold? */
	if (dfp->maxHemi <= specthresh+FTINY) {
		if (dfp->maxHemi > FTINY) {	/* XXX no color from BSDF */
			FVECT	vjit;
			double	d;
			COLOR	ctmp;
			bsdf_jitter(vjit, ndp, ndp->sr_vpsa[1]);
			d = SDdirectHemi(vjit, sflags, ndp->sd);
			if (sflags == SDsampSpT) {
				copycolor(ctmp, ndp->pr->pcol);
				scalecolor(ctmp, d);
			} else			/* no pattern on reflection */
				setcolor(ctmp, d, d, d);
			addcolor(unsc, ctmp);
		}
		return(0);
	}
						/* else need to sample */
	dimlist[ndims++] = (int)(size_t)ndp->mp;
	ndims++;
	for (n = dfp->ncomp; n--; ) {		/* loop over components */
		dimlist[ndims-1] = n + 9438;
		ntotal += sample_sdcomp(ndp, &dfp->comp[n], sflags==SDsampSpT);
	}
	ndims -= 2;
	return(ntotal);
}

/* Color a ray that hit a BSDF material */
int
m_bsdf(OBJREC *m, RAY *r)
{
	int	hitfront;
	COLOR	ctmp;
	SDError	ec;
	FVECT	upvec, vtmp;
	MFUNC	*mf;
	BSDFDAT	nd;
						/* check arguments */
	if ((m->oargs.nsargs < 6) | (m->oargs.nfargs > 9) |
				(m->oargs.nfargs % 3))
		objerror(m, USER, "bad # arguments");
						/* record surface struck */
	hitfront = (r->rod > 0);
						/* load cal file */
	mf = getfunc(m, 5, 0x1d, 1);
	setfunc(m, r);
						/* get thickness */
	nd.thick = evalue(mf->ep[0]);
	if ((-FTINY <= nd.thick) & (nd.thick <= FTINY))
		nd.thick = .0;
						/* check backface visibility */
	if (!hitfront & !backvis) {
		raytrans(r);
		return(1);
	}
						/* check other rays to pass */
	if (nd.thick != 0 && (r->crtype & SHADOW ||
				!(r->crtype & (SPECULAR|AMBIENT)) ||
				(nd.thick > 0) ^ hitfront)) {
		raytrans(r);			/* hide our proxy */
		return(1);
	}
	nd.mp = m;
	nd.pr = r;
						/* get BSDF data */
	nd.sd = loadBSDF(m->oargs.sarg[1]);
						/* early shadow check */
	if (r->crtype & SHADOW && (nd.sd->tf == NULL) & (nd.sd->tb == NULL))
		return(1);
						/* diffuse reflectance */
	if (hitfront) {
		cvt_sdcolor(nd.rdiff, &nd.sd->rLambFront);
		if (m->oargs.nfargs >= 3) {
			setcolor(ctmp, m->oargs.farg[0],
					m->oargs.farg[1],
					m->oargs.farg[2]);
			addcolor(nd.rdiff, ctmp);
		}
	} else {
		cvt_sdcolor(nd.rdiff, &nd.sd->rLambBack);
		if (m->oargs.nfargs >= 6) {
			setcolor(ctmp, m->oargs.farg[3],
					m->oargs.farg[4],
					m->oargs.farg[5]);
			addcolor(nd.rdiff, ctmp);
		}
	}
						/* diffuse transmittance */
	cvt_sdcolor(nd.tdiff, &nd.sd->tLamb);
	if (m->oargs.nfargs >= 9) {
		setcolor(ctmp, m->oargs.farg[6],
				m->oargs.farg[7],
				m->oargs.farg[8]);
		addcolor(nd.tdiff, ctmp);
	}
						/* get modifiers */
	raytexture(r, m->omod);
						/* modify diffuse values */
	multcolor(nd.rdiff, r->pcol);
	multcolor(nd.tdiff, r->pcol);
						/* get up vector */
	upvec[0] = evalue(mf->ep[1]);
	upvec[1] = evalue(mf->ep[2]);
	upvec[2] = evalue(mf->ep[3]);
						/* return to world coords */
	if (mf->fxp != &unitxf) {
		multv3(upvec, upvec, mf->fxp->xfm);
		nd.thick *= mf->fxp->sca;
	}
	if (r->rox != NULL) {
		multv3(upvec, upvec, r->rox->f.xfm);
		nd.thick *= r->rox->f.sca;
	}
	raynormal(nd.pnorm, r);
						/* compute local BSDF xform */
	ec = SDcompXform(nd.toloc, nd.pnorm, upvec);
	if (!ec) {
		nd.vray[0] = -r->rdir[0];
		nd.vray[1] = -r->rdir[1];
		nd.vray[2] = -r->rdir[2];
		ec = SDmapDir(nd.vray, nd.toloc, nd.vray);
	}
	if (ec) {
		objerror(m, WARNING, "Illegal orientation vector");
		return(1);
	}
	compute_through(&nd);			/* compute through component */
	if (r->crtype & SHADOW) {
		RAY	tr;			/* attempt to pass shadow ray */
		if (rayorigin(&tr, TRANS, r, nd.cthru) < 0)
			return(1);		/* blocked */
		VCOPY(tr.rdir, r->rdir);
		rayvalue(&tr);			/* transmit with scaling */
		multcolor(tr.rcol, tr.rcoef);
		copycolor(r->rcol, tr.rcol);
		return(1);			/* we're done */
	}
	ec = SDinvXform(nd.fromloc, nd.toloc);
	if (!ec)				/* determine BSDF resolution */
		ec = SDsizeBSDF(nd.sr_vpsa, nd.vray, NULL,
					SDqueryMin+SDqueryMax, nd.sd);
	if (ec)
		objerror(m, USER, transSDError(ec));

	nd.sr_vpsa[0] = sqrt(nd.sr_vpsa[0]);
	nd.sr_vpsa[1] = sqrt(nd.sr_vpsa[1]);
	if (!hitfront) {			/* perturb normal towards hit */
		nd.pnorm[0] = -nd.pnorm[0];
		nd.pnorm[1] = -nd.pnorm[1];
		nd.pnorm[2] = -nd.pnorm[2];
	}
						/* sample reflection */
	sample_sdf(&nd, SDsampSpR);
						/* sample transmission */
	sample_sdf(&nd, SDsampSpT);
						/* compute indirect diffuse */
	if (bright(nd.rdiff) > FTINY) {		/* ambient from reflection */
		if (!hitfront)
			flipsurface(r);
		copycolor(ctmp, nd.rdiff);
		multambient(ctmp, r, nd.pnorm);
		addcolor(r->rcol, ctmp);
		if (!hitfront)
			flipsurface(r);
	}
	if (bright(nd.tdiff) > FTINY) {		/* ambient from other side */
		FVECT  bnorm;
		if (hitfront)
			flipsurface(r);
		bnorm[0] = -nd.pnorm[0];
		bnorm[1] = -nd.pnorm[1];
		bnorm[2] = -nd.pnorm[2];
		copycolor(ctmp, nd.tdiff);
		if (nd.thick != 0) {		/* proxy with offset? */
			VCOPY(vtmp, r->rop);
			VSUM(r->rop, vtmp, r->ron, nd.thick);
			multambient(ctmp, r, bnorm);
			VCOPY(r->rop, vtmp);
		} else
			multambient(ctmp, r, bnorm);
		addcolor(r->rcol, ctmp);
		if (hitfront)
			flipsurface(r);
	}
						/* add direct component */
	if ((bright(nd.tdiff) <= FTINY) & (nd.sd->tf == NULL) &
					(nd.sd->tb == NULL)) {
		direct(r, dir_brdf, &nd);	/* reflection only */
	} else if (nd.thick == 0) {
		direct(r, dir_bsdf, &nd);	/* thin surface scattering */
	} else {
		direct(r, dir_brdf, &nd);	/* reflection first */
		VCOPY(vtmp, r->rop);		/* offset for transmitted */
		VSUM(r->rop, vtmp, r->ron, -nd.thick);
		direct(r, dir_btdf, &nd);	/* separate transmission */
		VCOPY(r->rop, vtmp);
	}
						/* clean up */
	SDfreeCache(nd.sd);
	return(1);
}
