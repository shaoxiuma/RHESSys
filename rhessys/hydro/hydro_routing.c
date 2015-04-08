/*--------------------------------------------------------------------------*/
/*  NAME                                                                    */
/*                                                                          */
/*                                                                          */
/*  SYNOPSIS                                                                */
/*      Integrated routing package:                                         */
/*          subsurface routing                                              */
/*          canopy-routing                                                  */
/*          surface routing (kinematic)                                     */
/*          stream-network routing (tbd)                                    */
/*          vertical ground-water processes                                 */
/*                                                                          */
/*  CONTAINS                                                                */
/*      void hydro_routing()                                                */
/*          main driver-routine                                             */
/*                                                                          */
/*      static void init_hydro_routing()                                    */
/*          allocates working data structures and computes                  */
/*          time independent sfcknl, sfccnti, sfcndxi, sfcgam               */
/*                                                                          */
/*      static void sub_routing()                                           */
/*          horizontal groundwater routing; determine coupling timestep     */
/*                                                                          */
/*      static void sub_vertical()                                          */
/*          infiltration, groundwater-balancing, and exfiltration           */
/*                                                                          */
/*      static void can_routing()                                           */
/*          canopy and precipitation rates                                  */
/*                                                                          */
/*      static void sfc_routing()                                           */
/*          copy initial state-data into working head and chem vectors;     */
/*          loop on adaptive internal time steps                            */
/*              parallel loop computing the  d(head)/dt and d(chem)/dt      */
/*              compute Courant-stable time step dt                         */
/*              parallel loop updating the head and chem for time dt        */
/*          update state-data from working vectors                          */
/*                                                                          */
/*      static void stream_routing()                                        */
/*          lateral inflow from surface                                     */
/*          baseflow                                                        */
/*          stream-network routing                                          */
/*                                                                          */
/*  DESCRIPTION:  sfc_routing()                                             */
/*      Uses an implementation based on "inflow matrices" and an            */
/*      adaptive time step.  For each cell and for surface-head and for     */
/*      each species chem in {H)3, NH4, DOC, DON}:                          */
/*                                                                          */
/*      We represent head and chem as column-vectors.                       */
/*                                                                          */
/*      hh  = head - retdep [M]                                             */
/*      vel = hh^(2/3) * sqrt( slope ) / ( dx * roughness ) [cells/S]       */
/*      d(head)/dt = vel * hh                                               */
/*      d(chem)/dt = vel * chem * ( hh / head )                             */
/*                                                                          */
/*      Note time-independent factor                                        */
/*      sfcknl = sqrt( slope ) / ( dx * roughness )                         */
/*                                                                          */
/*      Then for each source-cell S and receptor-cell R there is an         */
/*      exchange fraction gamma(S,R), so that the state-update equations    */
/*      are:                                                                */
/*                                                                          */
/*      head(R) = head(R) - dt * d(head)/dt(R)                              */
/*        + dt * sum{ gamma(S,R) * d(head)/dt(S) * area(S) } / area(R)      */
/*                                                                          */
/*      and similarly for each species chem.                                */
/*                                                                          */
/*      Again, note the time independent indexing arrays and factors        */
/*      sfccnti(R)   = number     of sources that flow into cell R          */
/*      sfcndxi(R,S) = subscripts of sources that flow into cell R           */
/*      sfcgam(R,S)  = gamma(S,R) * area(S) } / area(R)                      */
/*                                                                          */
/*      The "inflow matrices" approach is necessary for a parallel          */
/*      implementation, since we need a single point of update for          */
/*      the state at each patch:  it is unsafe for one patch to update      */
/*      the state of its downhill neighbors:  more than one patch might     */
/*      be updating any one of these neighbors simultaneously, leading      */
/*      to race conditions and incorrect results.                           */
/*                                                                          */
/*  PROGRAMMER NOTES                                                        */
/*      Initial version 3/12/2015 by Carlie J. Coats, Jr., UNC IE           */
/*--------------------------------------------------------------------------*/
#include <stdio.h>
#include <math.h>
#include "rhessys.h"

/*	Local function definitions.				*/

extern void  * alloc( size_t, char *, char *);

double	compute_z_final( int	verbose_flag,
						 double	p_0,
						 double	p,
						 double	soil_depth,
						 double	z_initial,
						 double	delta_water);

/*  MAXNEIGHBOR should be a multiple of 4, for memory-alignment reasons */
/*  EPSILON     used for roundoff-tolerance criterion (sec)  = 10 usec  */

#define     MAXNEIGHBOR     (16)
#define     TWOTHD          (2.0/3.0)
#define     DEG2RAD         (M_PI/180.0)
#define     EPSILON         (1.0e-5)

typedef     double    BDYdble[ MAXNEIGHBOR ] ;
typedef     unsigned  BDYuint[ MAXNEIGHBOR ] ;

static double   CPLMAX ;        /*  Courant-stability threshold                                 */
static double   COUMAX ;        /*  Max coupling timestep (sec; returned by sub_routing() )     */

static int      verbose ;

static unsigned num_patches = -9999 ;

static double   basin_area ;
static double   std_scale  ;

struct patch_object * * plist ; /*  array of pointers plist[num_patches] to the patches     */

static double * psize ;         /*  cell-size:  sqrt( patch->area )                         */
static double * pscale;         /*  patch->std  * std_scale                                 */
static double * perim ;         /*  patch->perimeter                                        */

static unsigned * nsoil ;       /*  patch->num_soil_intervals                               */
static double   * dzsoil ;      /*  patch->soil_defaults[0][0].interval_size                */

static double * retdep ;        /*  patch->soil_defaults[0][0].detention_store_size         */
static double * rootzs ;        /*  patch->rootzone.S or patch->S                           */
static double * ksatv  ;        /*  patch->Ksat_vertical                                    */
static double * ksat_0 ;        /*  patch->soil_defaults[0][0].Ksat_0_v                     */
static double * mz_v   ;        /*  patch->soil_defaults[0][0].mz_v                         */
static double * por_0  ;        /*  patch->Ksoil_defaults[0][0].porosity_0                  */
static double * por_d  ;        /*  patch->soil_defaults[0][0].porosity_decay               */
static double * psiair ;        /*  patch->soil_defaults[0][0].psi_air_entry                */
static double * zsoil  ;        /*  patch->soil_defaults[0][0].soil_depth                   */
static double * Ndecay ;        /*  patch->soil_defaults[0][0].N_decay_rate                 */
static double * Ddecay ;        /*  patch->soil_defaults[0][0].DOM_decay_rate               */

static double * waterz ;        /*  water table elevation (vertical M)                      */

static double * capH2O ;        /*  patch->field_capacity                                   */
static double * totH2O ;        /*  total column water (vertical M)                         */
static double * totNO3 ;        /*  total column NO3                                        */
static double * totNH4 ;        /*  total column NH4                                        */
static double * totDON ;        /*  total column DON                                        */
static double * totDOC ;        /*  total column DOC                                        */

static double * sfcH2O ;        /*  patch->detention_store water                            */
static double * sfcNO3 ;        /*  patch->surface_NO3                                      */
static double * sfcNH4 ;        /*  patch->surface_NH4                                      */
static double * sfcDOC ;        /*  patch->surface_DOC                                      */
static double * sfcDON ;        /*  patch->surface_DON                                      */
static double * sfcknl ;        /*  time-independent factor in surface velocity calc.       */

static double * infH2O ;        /*  H2O infiltration                                        */
static double * infNO3 ;        /*  NO3 infiltration                                        */
static double * infNH4 ;        /*  NH4 infiltration                                        */
static double * infDOC ;        /*  DOC infiltration                                        */
static double * infDON ;        /*  DON infiltration                                        */

static double * latH2O ;        /*  H2O lateral flow from sub_route()                       */
static double * latNO3 ;        /*  NO3 lateral flow                                        */
static double * latNH4 ;        /*  NH4 lateral flow                                        */
static double * latDOC ;        /*  DOC lateral flow                                        */
static double * latDON ;        /*  DON lateral flow                                        */

static double * canH2O ;        /*  H2O from can_route() to surface (m/s)                   */
static double * canNO3 ;        /*  NO3 from can_route() to surface (?/s)                   */
static double * canNH4 ;        /*  NH4 from can_route() to surface                         */
static double * canDOC ;        /*  DOC from can_route() to surface                         */
static double * canDON ;        /*  DON from can_route() to surface                         */

/*  Surface-routing Drainage Matrix  */

static unsigned * sfccnti ;     /*   used as sfccnti[num_patches]                */
static BDYuint  * sfcndxi ;     /*   used as sfcndxi[num_patches][MAXNEIGHBOR]:  outflow-subscripts   */
static BDYdble  * sfcgam ;      /*   used as  sfcgam[num_patches][MAXNEIGHBOR]   */

/*  Sub-Surface-routing Drainage Matrix  */

static unsigned * subcnto ;     /*   used as  subcnto[num_patches]:                outflow count        */
static unsigned * subcnti ;     /*   used as  subcnti[num_patches]:                 inflow count        */
static BDYuint  * subndxo ;     /*   used as  subndxo[num_patches][MAXNEIGHBOR]:   outflow subscripts   */
static BDYuint  * subndxi ;     /*   used as  subndxi[num_patches][MAXNEIGHBOR]:    inflow subscripts   */
static BDYdble  * perimf  ;     /*   used as   perimf[num_patches][MAXNEIGHBOR]   */
static BDYdble  * subdist ;     /*   used as  subdist[num_patches][MAXNEIGHBOR]   */

static double   normal[9] = { 0.0, 0.253, 0.524, 0.842, 1.283, -0.253, -0.524, -0.842, -1.283 };
static double     perc[9] = { 0.2, 0.1,   0.1,   0.1,   0.1,    0.1,    0.1,    0.1,    0.1   };   


/*--------------------------------------------------------------------------*/ 


static unsigned patchdex( struct patch_object * patch )   /*  patch-subscript in plist[]  */
    {
    unsigned    i ;
    for ( i = 0 ; i < num_patches; i++ )
        {
        if ( patch == plist[i] )  return( i ) ;
        }
    return( num_patches+1 ) ;
    }


/*--------------------------------------------------------------------------*/

static void init_hydro_routing( struct command_line_object * command_line,
                                struct basin_object        * basin )
    {
    unsigned                i, j, k, m, n ;
    double                  gfac, dx, dy, diagf ;
    unsigned                dcount[basin->route_list->num_patches] ;    /*  array of surface-downhill-neighbor counts  */
    double                  dfrac [basin->route_list->num_patches][MAXNEIGHBOR] ;
	struct patch_object *   patch ;
	struct patch_object *   neigh ;
    
    verbose   = command_line->verbose_flag ;
    std_scale = command_line->std_scale ;

    CPLMAX    = 1800.0 ;                    /*  max hydro coupling time step (sec)  */
    COUMAX    =    0.2 ;                    /*  max Courant condition (number)      */

    num_patches = basin->route_list->num_patches ;

    plist   = (struct patch_object * *) alloc( num_patches * sizeof(struct patch_object *), "plist", "hydro_routing/init_hydro_routing()" ) ;

    psize   = (double   *) alloc( num_patches * sizeof(   double ), "psize",  "hydro_routing/init_hydro_routing()" ) ;
    nsoil   = (unsigned *) alloc( num_patches * sizeof(   double ), "nsoil",  "hydro_routing/init_hydro_routing()" ) ;
    dzsoil  = (double   *) alloc( num_patches * sizeof(   double ), "dzsoil", "hydro_routing/init_hydro_routing()" ) ;
    pscale  = (double   *) alloc( num_patches * sizeof(   double ), "pscale", "hydro_routing/init_hydro_routing()" ) ;

    retdep  = (double   *) alloc( num_patches * sizeof(   double ), "retdep", "hydro_routing/init_hydro_routing()" ) ;
    rootzs  = (double   *) alloc( num_patches * sizeof(   double ), "rootzs", "hydro_routing/init_hydro_routing()" ) ;
    ksatv   = (double   *) alloc( num_patches * sizeof(   double ), "ksatv",  "hydro_routing/init_hydro_routing()" ) ;
    ksat_0  = (double   *) alloc( num_patches * sizeof(   double ), "ksat_0", "hydro_routing/init_hydro_routing()" ) ;
    mz_v    = (double   *) alloc( num_patches * sizeof(   double ), "mz_v",   "hydro_routing/init_hydro_routing()" ) ;
    por_0   = (double   *) alloc( num_patches * sizeof(   double ), "por_0",  "hydro_routing/init_hydro_routing()" ) ;
    por_d   = (double   *) alloc( num_patches * sizeof(   double ), "por_d",  "hydro_routing/init_hydro_routing()" ) ;
    psiair  = (double   *) alloc( num_patches * sizeof(   double ), "psiair", "hydro_routing/init_hydro_routing()" ) ;
    zsoil   = (double   *) alloc( num_patches * sizeof(   double ), "zsoil",  "hydro_routing/init_hydro_routing()" ) ;
    zsoil   = (double   *) alloc( num_patches * sizeof(   double ), "zsoil",  "hydro_routing/init_hydro_routing()" ) ;
    Ndecay  = (double   *) alloc( num_patches * sizeof(   double ), "Ndecay", "hydro_routing/init_hydro_routing()" ) ;
    Ddecay  = (double   *) alloc( num_patches * sizeof(   double ), "Ddecay", "hydro_routing/init_hydro_routing()" ) ;

    waterz  = (double   *) alloc( num_patches * sizeof(   double ), "waterz", "hydro_routing/init_hydro_routing()" ) ;

    capH2O  = (double   *) alloc( num_patches * sizeof(   double ), "capH2O", "hydro_routing/init_hydro_routing()" ) ;

    totH2O  = (double   *) alloc( num_patches * sizeof(   double ), "totH2O", "hydro_routing/init_hydro_routing()" ) ;
    totNO3  = (double   *) alloc( num_patches * sizeof(   double ), "totNO3", "hydro_routing/init_hydro_routing()" ) ;
    totNH4  = (double   *) alloc( num_patches * sizeof(   double ), "totNH4", "hydro_routing/init_hydro_routing()" ) ;
    totDOC  = (double   *) alloc( num_patches * sizeof(   double ), "totDOC", "hydro_routing/init_hydro_routing()" ) ;
    totDON  = (double   *) alloc( num_patches * sizeof(   double ), "totDON", "hydro_routing/init_hydro_routing()" ) ;

    infH2O  = (double   *) alloc( num_patches * sizeof(   double ), "infH2O", "hydro_routing/init_hydro_routing()" ) ;
    infNO3  = (double   *) alloc( num_patches * sizeof(   double ), "infNO3", "hydro_routing/init_hydro_routing()" ) ;
    infNH4  = (double   *) alloc( num_patches * sizeof(   double ), "infNH4", "hydro_routing/init_hydro_routing()" ) ;
    infDOC  = (double   *) alloc( num_patches * sizeof(   double ), "infDOC", "hydro_routing/init_hydro_routing()" ) ;
    infDON  = (double   *) alloc( num_patches * sizeof(   double ), "infDON", "hydro_routing/init_hydro_routing()" ) ;

    latH2O  = (double   *) alloc( num_patches * sizeof(   double ), "latH2O", "hydro_routing/init_hydro_routing()" ) ;
    latNO3  = (double   *) alloc( num_patches * sizeof(   double ), "latNO3", "hydro_routing/init_hydro_routing()" ) ;
    latNH4  = (double   *) alloc( num_patches * sizeof(   double ), "latNH4", "hydro_routing/init_hydro_routing()" ) ;
    latDOC  = (double   *) alloc( num_patches * sizeof(   double ), "latDOC", "hydro_routing/init_hydro_routing()" ) ;
    latDON  = (double   *) alloc( num_patches * sizeof(   double ), "latDON", "hydro_routing/init_hydro_routing()" ) ;

    sfcH2O  = (double   *) alloc( num_patches * sizeof(   double ), "sfcH2O", "hydro_routing/init_hydro_routing()" ) ;
    sfcNO3  = (double   *) alloc( num_patches * sizeof(   double ), "sfcNO3", "hydro_routing/init_hydro_routing()" ) ;
    sfcNH4  = (double   *) alloc( num_patches * sizeof(   double ), "sfcNH4", "hydro_routing/init_hydro_routing()" ) ;
    sfcDOC  = (double   *) alloc( num_patches * sizeof(   double ), "sfcDOC", "hydro_routing/init_hydro_routing()" ) ;
    sfcDON  = (double   *) alloc( num_patches * sizeof(   double ), "sfcDON", "hydro_routing/init_hydro_routing()" ) ;

    canH2O  = (double   *) alloc( num_patches * sizeof(   double ), "canH2O", "hydro_routing/init_hydro_routing()" ) ;
    canNO3  = (double   *) alloc( num_patches * sizeof(   double ), "canNO3", "hydro_routing/init_hydro_routing()" ) ;
    canNH4  = (double   *) alloc( num_patches * sizeof(   double ), "canNH4", "hydro_routing/init_hydro_routing()" ) ;
    canDOC  = (double   *) alloc( num_patches * sizeof(   double ), "canDOC", "hydro_routing/init_hydro_routing()" ) ;
    canDON  = (double   *) alloc( num_patches * sizeof(   double ), "canDON", "hydro_routing/init_hydro_routing()" ) ;

    sfcknl  = (double   *) alloc( num_patches * sizeof(   double ), "sfcknl", "hydro_routing/init_hydro_routing()" ) ;
    sfccnti = (unsigned *) alloc( num_patches * sizeof( unsigned ), "sfccnti", "hydro_routing/init_hydro_routing()" ) ;
    sfcndxi = (BDYuint  *) alloc( num_patches * sizeof(  BDYuint ), "sfcndxi", "hydro_routing/init_hydro_routing()" ) ;
    sfcgam  = (BDYdble  *) alloc( num_patches * sizeof(  BDYdble ), "sfcgam",  "hydro_routing/init_hydro_routing()" ) ;

    subcnto = (unsigned *) alloc( num_patches * sizeof( unsigned ), "subcnto", "hydro_routing/init_hydro_routing()" ) ;
    subcnti = (unsigned *) alloc( num_patches * sizeof( unsigned ), "subcnti", "hydro_routing/init_hydro_routing()" ) ;
    subndxo = (BDYuint  *) alloc( num_patches * sizeof(  BDYuint ), "subndxo", "hydro_routing/init_hydro_routing()" ) ;
    subndxi = (BDYuint  *) alloc( num_patches * sizeof(  BDYuint ), "subndxi", "hydro_routing/init_hydro_routing()" ) ;
    perimf  = (BDYdble  *) alloc( num_patches * sizeof(  BDYdble ), "perimf",  "hydro_routing/init_hydro_routing()" ) ;
    subdist = (BDYdble  *) alloc( num_patches * sizeof(  BDYdble ), "subdist", "hydro_routing/init_hydro_routing()" ) ;

    diagf = 0.5 * sqrt( 0.5 ) ;     /*  "perimeter" factor for diagonals */
    basin_area = 0.0 ;
    
#pragma omp parallel for                                                \
        default( none )                                                 \
        private( i, j, k, patch, neigh, gfac, dx, dy )                  \
         shared( num_patches, basin, plist, psize, sfccnti, subcnto,    \
                 retdep, rootzs, ksatv, ksat_0, mz_v, psiair, zsoil,    \
                 nsoil, dzsoil, std_scale, pscale, Ndecay, Ddecay,      \
                 sfcknl, dcount, dfrac, capH2O, por_0, por_d,           \
                 subdist, subndxo, perimf, diagf, subcnti )             \
      reduction( +:  basin_area )

    for ( i = 0; i < num_patches; i++ )
        {
        patch     = basin->route_list->list[i] ;
        plist [i] = patch ;
        capH2O[i] = patch->field_capacity ;
        psize [i] = sqrt( patch->area ) ;
        nsoil [i] = patch->num_soil_intervals ;
        dzsoil[i] = patch->soil_defaults[0][0].interval_size ;
        pscale[i] = std_scale * patch->std ;
        retdep[i] = patch->soil_defaults[0][0].detention_store_size ;
        rootzs[i] = ( patch->rootzone.depth > ZERO ? patch->rootzone.S : patch->S ) ;
        ksatv [i] = patch->Ksat_vertical ;
        ksat_0[i] = patch->soil_defaults[0][0].Ksat_0_v ;
        mz_v  [i] = patch->soil_defaults[0][0].mz_v ;
        por_0 [i] = patch->soil_defaults[0][0].porosity_0 ;
        por_d [i] = patch->soil_defaults[0][0].porosity_decay ;
        psiair[i] = patch->soil_defaults[0][0].psi_air_entry ;
        zsoil [i] = patch->soil_defaults[0][0].soil_depth ;
        Ndecay[i] = patch->soil_defaults[0][0].N_decay_rate ;
        Ddecay[i] = patch->soil_defaults[0][0].DOM_decay_rate ;
        sfcknl[i] = sqrt( tan( patch->slope_max ) ) / ( patch->mannN * psize[i] ) ;
        dcount[i] = plist[i]->surface_innundation_list->num_neighbours ;

        sfccnti[i] = 0 ;
        subcnto[i] = patch->innundation_list->num_neighbours ;
        subcnti[i] = 0 ;

        gfac = 0.0 ;
        for ( j = 0; j < dcount[i]; j++ )       /*  compute normalized outflow-fractions  */
            {
            gfac += plist[k]->surface_innundation_list->neighbours[j].gamma ;
            }
        gfac = 1.0 / gfac ;
        for ( j = 0; j < dcount[i]; j++ )       /*  compute normalized outflow-fractions from         */
            {                                   /*  flow-rates gamma and uphill/downhill area ratios  */
            neigh = plist[i]->surface_innundation_list->neighbours[j].patch;
            dfrac[i][j] = gfac * plist[k]->surface_innundation_list->neighbours[j].gamma  * plist[i]->area / neigh->area ;
            }
        
        for ( j = 0; j < subcnto[i]; j++ )
            {
            neigh = plist[i]->innundation_list->neighbours[j].patch;
            dx    = neigh->x - plist[j]->x ;
            dy    = neigh->y - plist[j]->y ;
            subdist[i][j] = sqrt( dx*dx + dy*dy )  ;
            subndxo[i][j] = patchdex( neigh ) ;
            if ( dx+dy < 1.1*subdist[i][j] )
                {
                perimf[i][j] = diagf * plist[i]->area / neigh->area ;    /* diagonal-direction factor */
                }
            else{
                perimf[i][j] = 0.5 * plist[i]->area / neigh->area ;     /* along-axis factor  */
                }
            }
        }                   /*  end first (parallel) initialization loop  */

    /*  !! Serial loop !!  -- computing inflow-neighbor tables and matrices  */

    for ( i = 0; i < num_patches; i++ )
        {
        
        /*  invert the surface-routing table  */
        
        for ( j = 0; j < dcount[i]; j++ )
            {
            neigh = plist[i]->surface_innundation_list->neighbours[j].patch ;
            k = patchdex( neigh ) ;
            if ( sfccnti[k] < MAXNEIGHBOR-1 )
                {
                m = sfccnti[k] ;
                sfccnti[k]++ ;
                sfcndxi[k][m] = j ;
                sfcgam [k][m] = dfrac[k][j] ;
                break ;
                }
            else{
                fprintf( stderr, "ERROR:  matrix-overflow in hydro_routing.c:  increase MAXNEIGHBOR and re-=compile" );
                exit(EXIT_FAILURE);
                }
            }
        
        /*  use existing subsurface-routing table:  need distances, area-ratios for neighbors  */
        
        for ( j = 0; j < subcnto[i]; j++ )
            {
            neigh = plist[ subndxo[i][j] ] ;
            k     = patchdex( neigh ) ;
            m     = MAXNEIGHBOR * k + subcnti[k]  ;
            subndxi[k][j] = i ;
            subcnti[k]++  ;
            }

        }       /*  end serial loop constructing drainage matrices  */

    return ;

    }           /*  end init_hydro_routing()  */


/*--------------------------------------------------------------------------*/

static void sub_routing( double   tstep,        /*  external time step      */
                         double * substep )     /*  hydro-coupling time step: <= min( CPLMAX, tstep )  */
    {
    double      z1, z2, zz, vel, slope, cmax, dt ;
    double      ss, std, tsum, gsum, wsum, fac ;
    double      dH2O, dNO3, dNH4, dDOC, dDON ;
    unsigned    i, j, k, kk, m, mm ;
    int         n ;
    double      trans [num_patches] ;
    double      outfac[num_patches] ;
    double      outH2O[num_patches] ;
    double      dH2Odt[num_patches][MAXNEIGHBOR] ;
    double      rtefac[num_patches][MAXNEIGHBOR] ;
    double      gamma [num_patches][MAXNEIGHBOR] ;
	struct patch_object *   patch ;
	struct patch_object *   neigh ;

#pragma omp parallel for                    \
        default( none )                     \
        private( i, m, n, patch, zz, tsum ) \
         shared( num_patches, plist, pscale, nsoil, dzsoil, normal, perc, trans )

    for ( i = 0; i < num_patches; i++ )     /*  calculate  water-table Z, transmissivity  */
        {
        patch = plist[i] ;        
        if ( pscale[i] > 0 )
            {
            tsum = 0.0 ;
            for ( m = 0 ; m < 9 ; m++ )
                {
                n = min( (int)nsoil[i], (int) lround( patch->sat_deficit + normal[m]*pscale[i])/dzsoil[i] ) ;
                tsum += patch->transmissivity_profile[n] * perc[m] ;
                }
            trans[i] = tsum ;
            }
        else{
            n        = min( (int)nsoil[i], (int)lround( patch->sat_deficit/dzsoil[i] ) ) ;
            trans[i] = patch->transmissivity_profile[n] ;
            }
        }           /*  end loop on water-table Z, transmissivity  */

        cmax = COUMAX / min( tstep, CPLMAX ) ;             /*  "Courant-stable for one time-step"  */

#pragma omp parallel for                                            \
        default( none )                                             \
        private( i, j, kk, patch, z1, z2, zz, slope, gsum, wsum,    \
                 fac, vel )                                         \
         shared( num_patches, plist, pscale, nsoil, dzsoil, perimf, \
                 waterz, dH2Odt, outH2O, gamma, subcnto, subndxo,   \
                 subdist, trans, psize )                            \
      reduction( max: cmax )                                        \
       schedule( guided )

    for ( i = 0; i < num_patches; i++ )           /*  calculate  dH2Odt[]  */
        {
        patch = plist[i] ;
        kk    = num_patches+1 ;
        z1    = waterz[i]  ;
        gsum = 0.0 ;
        wsum = 0.0 ;
        for ( j = 0; j < subcnto[i]; j++ )     /*  find max (positive-) slope for outflow  */
            {
            kk    = subndxo[i][j] ;
            z2    = waterz[kk] ;
            slope = ( z1 - z2 ) / subdist[i][j] ;
            if ( slope > ZERO )
                {
                zz           = 0.5 * ( z1 + z2 ) ;
                vel          = slope * trans[i] / psize [i] ;                /*  cells/sec  */
                gamma [i][j] = slope ;
                dH2Odt[i][j] = perimf[i][j] * zz * vel ;                    /*  outflow  */
                gsum         = gsum + gamma [i][j] ;
                wsum         = wsum + dH2Odt[i][j] ;
                if ( vel > cmax )  cmax = vel ;
                }
            else{
                dH2Odt[i][j] = 0.0 ;
                gamma [i][j] = 0.0 ;
                }
            }
        outH2O[i] = wsum ;
        if ( gsum > ZERO )     /*  Normalize gamma[i][:]  */
            {
            gsum = 1.0 / gsum ;
            for ( j = 0; j < subcnto[i]; j++ )
                {
                gamma[i][j] = gsum * gamma[i][j] ;
                };
            }
        }           /*  end loop calculating  dH2Odt[]  */

    *substep = dt = min( COUMAX / cmax , tstep ) ;

#pragma omp parallel for                                            \
        default( none )                                             \
        private( i, j, m, fac )                                     \
         shared( num_patches, plist, outfac, outH2O,  subcnto,      \
                 totH2O, dH2Odt, gamma, dt, rtefac )                \
       schedule( guided )

    for ( i = 0; i < num_patches; i++ )       /*  calculate fraction of water leavng to each neighbor */
        {
        fac = dt / totH2O[i] ;
        outfac[i] = fac * outH2O[i] ;
        for ( j = 0; j < subcnto[i]; j++ )
            {
            m = j + MAXNEIGHBOR * i ;
            rtefac[i][j] = fac * gamma[i][j] * dH2Odt[i][j] ;  /* ?? - fraction of H2O that leaves plist[i] in direction j */
            }
        }       /*  end loop calculating fraction of water leavng  */

#pragma omp parallel for                                            \
        default( none )                                             \
        private( i, j, k, dH2O, dNO3, dNH4, dDOC, dDON )            \
         shared( num_patches, plist, subcnti, subndxi,              \
                 dt, outfac, dH2Odt, rtefac,                        \
                 outH2O, totNO3, totNH4, totDON, totDOC,            \
                 latH2O, latNO3, latNH4, latDON, latDOC )           \
       schedule( guided )

    for ( i = 0; i < num_patches; i++ )     /*  update H2O, NO3, NH4, DON, DOC  */
        {
        dH2O = -outH2O[i] * dt ;
        dNO3 = -outfac[i] * totNO3[i] ;
        dNH4 = -outfac[i] * totNH4[i] ;
        dDON = -outfac[i] * totDON[i] ;
        dDOC = -outfac[i] * totDOC[i] ;
        for ( j = 0; j < subcnti[i]; j++ )
            {
            k     = subndxi[i][j] ;
            dH2O += dH2Odt[k][j] * dt ;
            dNO3 += rtefac[k][j] * totNO3[i] ;
            dNH4 += rtefac[k][j] * totNH4[i] ;
            dDON += rtefac[k][j] * totDON[i] ;
            dDOC += rtefac[k][j] * totDOC[i] ;
            }
        latH2O[i] = dH2O ;
        latNO3[i] = dNO3 ;
        latNH4[i] = dNH4 ;
        latDON[i] = dDON ;
        latDOC[i] = dDOC ;
        }       /*  end loop updating state-variables  */
    
    return ;

    }           /*  end sub_routing()  */


/*--------------------------------------------------------------------------*/

static void can_routing( double  tstep )        /*  process time-step  */
    {
    unsigned                i ;
	struct patch_object *   patch ;

    /*  Initialize canopy rates: */

#pragma omp parallel for    \
        default( none )     \
        private( i )        \
         shared( num_patches, canH2O, canNO3, canNH4, canDOC, canDON )
    for ( i = 0; i < num_patches; i++ )
        {
        canH2O[i] = 0.0 ;
        canNO3[i] = 0.0 ;
        canNH4[i] = 0.0 ;
        canDOC[i] = 0.0 ;
        canDON[i] = 0.0 ;
        }

    /*  Add precip, fall-through: */

    return ;
    }           /*  end can_routing()  */


/*--------------------------------------------------------------------------*/

static void sfc_routing( double  tstep )        /*  process time-step  */
    {
    unsigned                i, j, k, m ;
    double                  z, poro, ksat, Sp, psi_f, theta, intensity, tp, delta, afac  ;
    double                  t, tfinal, dt1, dt2, dt ;
    double                  hh, vel, div, cmax ;
    double                  sumH2O, sumNO3, sumNH4, sumDOC, sumDON ;
    double                  outH2O[num_patches] ;       /*  outflow rates  */
    double                  outNO3[num_patches] ;
    double                  outNH4[num_patches] ;
    double                  outDOC[num_patches] ;
    double                  outDON[num_patches] ;
	struct patch_object *   patch ;

    /*  Initialize infiltration: */

#pragma omp parallel for        \
        default( none )         \
        private( i )            \
         shared( num_patches, infH2O, infNO3, infNH4, infDOC, infDON )
    for ( i = 0; i < num_patches; i++ )
        {
        infH2O[i] = 0.0 ;
        infNO3[i] = 0.0 ;
        infNH4[i] = 0.0 ;
        infDOC[i] = 0.0 ;
        infDON[i] = 0.0 ;
        }

    /*  internal timestep loop: */

    tfinal = tstep - EPSILON ;                   /*  tolerance for round-off (10 usec)  */

    for ( t = 0.0 ; t < tfinal ; t+=dt )
        {
        cmax = COUMAX / tstep ;             /*  "Courant-stable for one external time-step"  */

#pragma omp parallel for                                                \
        default( none )                                                 \
        private( i, z, ksat, poro, Sp, psi_f,theta, hh, vel, div )      \
         shared( num_patches, retdep, sfcknl,                           \
                 sfcH2O, sfcNO3, sfcNH4, sfcDOC, sfcDON,                \
                 outH2O, outNO3, outNH4, outDOC, outDON )               \
        reduction( max: cmax )                                          \
        schedule( guided )

        for ( i = 0; i < num_patches; i++ ) /*  compute drainage rates  */
            {           

            /*  Transport  */

            hh = sfcH2O[i] - retdep[i] ;
            if ( hh > 0.0 )
                {
                vel = sfcknl[i] * pow( hh, TWOTHD ) ;  /*  vel units: cells per unit time  */
                div = hh / sfcH2O[i] ;
                outH2O[i] = vel * hh ;
                outNO3[i] = vel * div * sfcNO3[i] ;
                outNH4[i] = vel * div * sfcNH4[i] ;
                outDOC[i] = vel * div * sfcDOC[i] ;
                outDON[i] = vel * div * sfcDON[i] ;
                if ( vel > cmax )  cmax = vel ;
                }
            else{
                outH2O[i] = 0.0 ;
                outNO3[i] = 0.0 ;
                outNH4[i] = 0.0 ;
                outDOC[i] = 0.0 ;
                outDON[i] = 0.0 ;
                }
            }                           /*  end drainage rate loop  */

        dt = min( COUMAX / cmax , tstep - t ) ; /*  Courant-stable time step  */

#pragma omp parallel for                                                \
        default( none )                                                 \
        private( i, j, k, sumH2O, sumNO3, sumNH4, sumDOC, sumDON, z,    \
                 ksat, poro, theta, psi_f, Sp, intensity, tp, delta,    \
                 afac )                                                 \
         shared( num_patches, plist, mz_v, ksat_0, ksatv, por_d, por_0, \
                 dt, sfccnti, sfcndxi, sfcgam, rootzs, psiair,          \
                 sfcH2O, sfcNO3, sfcNH4, sfcDOC, sfcDON,                \
                 canH2O, canNO3, canNH4, canDOC, canDON,                \
                 infH2O, infNO3, infNH4, infDOC, infDON,                \
                 outH2O, outNO3, outNH4, outDOC, outDON )               \
       schedule( guided )

        for ( i = 0; i < num_patches; i++ )     /*  update&infiltration loop  */
            {
            
            /*  accumulate and apply net in-flows  */

            sumH2O = -outH2O[i] ;   /*  start with outflow rate  */
            sumNO3 = -outNO3[i] ;
            sumNH4 = -outNH4[i] ;
            sumDOC = -outDOC[i] ;
            sumDON = -outDON[i] ;
            for ( j = 0 ; j < sfccnti[i] ; j++ )     /*  add the inflow rates  */
                {
                k = sfcndxi[i][j] ;                
                sumH2O += sfcgam[i][j] * outH2O[k] ;
                sumNO3 += sfcgam[i][j] * outNO3[k] ;
                sumNH4 += sfcgam[i][j] * outNH4[k] ;
                sumDOC += sfcgam[i][j] * outDOC[k] ;
                sumDON += sfcgam[i][j] * outDON[k] ;
                }
            sumH2O += canH2O[i] ;       /*  add the can_route() rates  */
            sumNO3 += canNO3[i] ;
            sumNH4 += canNH4[i] ;
            sumDOC += canDOC[i] ;
            sumDON += canDON[i] ;
            sfcH2O[i] += sumH2O *dt ;   /*  update surface state  */
            sfcNO3[i] += sumNO3 *dt ;
            sfcNH4[i] += sumNH4 *dt ;
            sfcDOC[i] += sumDOC *dt ;
            sfcDON[i] += sumDON *dt ;

            /*  Calculate Infiltration  */
            
            if ( ( rootzs[i] < 1.0 ) && ( ksat_0[i] > ZERO ) )
                {
                z     = plist[i]->sat_deficit_z ;

	            /*	use mean K and p (porosity) given current saturation depth   */
                
                ksat  = ( mz_v[i] > ZERO   ? mz_v[i]*ksat_0[i]*(1.0 - exp(-z/mz_v[i]) )/z : ksat_0[i] ) ;
                poro  = ( por_d[i] < 999.9 ? por_d[i]*por_0[i]*(1.0 - exp(-z/por_d[i]))/z : por_0[i]  ) ;

                /*	soil moisture deficit - S must be converted to theta	*/
                
                theta = rootzs[i] * poro ;

                /*	estimate sorptivity					*/
                
                psi_f = 0.76 * psiair[i] ;
                Sp    = sqrt( 2.0 * ksat * psi_f ) ;
                intensity = sfcH2O[i] / dt ;
	            if (intensity > ksat)
                    {
                    tp = ksat *  psi_f * ( poro - theta ) / ( intensity * (intensity-ksat) ) ;
                    }
                else{
                    tp = dt ;
                    }
                    
	            /* use Ksat_vertical to limit infiltration only to pervious area */
                
                if ( dt <= tp )
                    {
                    delta = ksatv[i]*sfcH2O[i] ;
                    }
                else{
                    afac  = sqrt( ksat ) ;
                    afac  = afac*afac*afac / 3.0 ;
                    delta = Sp * sqrt( dt - tp ) + afac + tp * sfcH2O[i] ;
                    delta = ksatv[i]*( delta < sfcH2O[i] ? delta :  sfcH2O[i] ) ;
                    }
                    
	            /* Update surface and infiltration variables: */
                
                afac = delta / sfcH2O[i] ;      /*  new-infiltration fraction  */
                infH2O[i] += delta ;
                sfcH2O[i] -= delta ;
                infNO3[i] += afac * sfcNO3[i] ;
                sfcNO3[i] -= afac * sfcNO3[i] ;
                infNH4[i] += afac * sfcNH4[i] ;
                sfcNH4[i] -= afac * sfcNH4[i] ;
                infDOC[i] += afac * sfcDOC[i] ;
                sfcDOC[i] -= afac * sfcDOC[i] ;
                infDON[i] += afac * sfcDON[i] ;
                sfcDON[i] -= afac * sfcDON[i] ;

                }           /*  Calculate Infiltration : if ( rootzs[i] < 1.0 ) && ( ksat_0[i] > ZERO )  */

            }               /*  end &infiltration loop  */
            
        }                   /*  end internal timestep loop  */

    return ;

    }       /*  end sfc_routing()  */


/*--------------------------------------------------------------------------*/

static void stream_routing( double  tstep )        /*  process time-step  */
    {

    /*  scavenge lateral inflow  */

    /*  Main processing loop  */

    /*  copy overflow to surface  */

    return ;

    }           /*  end stream_routing()  */


/*--------------------------------------------------------------------------*/

static void sub_vertical( double  tstep )        /*  process time-step  */
    {
    unsigned                i, j ;
    double                  fac, dH2O ;
	struct patch_object *   patch ;

#pragma omp parallel for                                    \
        default( none )                                     \
        private( i, fac, dH2O )                             \
         shared( num_patches, capH2O, plist,                \
                 verbose, por_0, por_d, dzsoil, waterz,     \
                 totH2O, totNO3, totNH4, totDOC, totDON,    \
                 infH2O, infNO3, infNH4, infDOC, infDON,    \
                 latH2O, latNO3, latNH4, latDOC, latDON,    \
                 sfcH2O, sfcNO3, sfcNH4, sfcDOC, sfcDON )   \
       schedule( guided )

    for ( i = 0; i < num_patches; i++ )     /*  loop on patches  */
        {
        
        /*  Add infiltration, lateral inflow  */
             
        totH2O[i] = totH2O[i] + infH2O[i] + latH2O[i] ;
        totNO3[i] = totNO3[i] + infNO3[i] + latNO3[i] ;
        totNH4[i] = totNH4[i] + infNH4[i] + latNH4[i] ;
        totDON[i] = totDON[i] + infDON[i] + latDON[i] ;
        totDOC[i] = totDOC[i] + infDOC[i] + latDOC[i] ;
        
        /*  re-compute surface water  */
        
        if ( totH2O[i] > capH2O[i] )
            {
            fac = ( totH2O[i] - capH2O[i] ) / totH2O[i] ;       /*  excess-water fraction  */
            sfcH2O[i] = sfcH2O[i] + fac * totH2O[i]  ;
            sfcNO3[i] = sfcNO3[i] + fac * totNO3[i]  ;
            sfcNH4[i] = sfcNH4[i] + fac * totNH4[i]  ;
            sfcDON[i] = sfcDON[i] + fac * totDON[i]  ;
            sfcDOC[i] = sfcDOC[i] + fac * totDOC[i]  ;
            totH2O[i] = totH2O[i] - fac * totH2O[i]  ;
            totNO3[i] = totNO3[i] - fac * totNO3[i]  ;
            totNH4[i] = totNH4[i] - fac * totNH4[i]  ;
            totDON[i] = totDON[i] - fac * totDON[i]  ;
            totDOC[i] = totDOC[i] - fac * totDOC[i]  ;
            waterz[i] = plist[i]->z ;
            }
        else{
            dH2O      = totH2O[i] - capH2O[i] ;
            waterz[i] = plist[i]->z - compute_z_final( verbose, por_0[i], por_d[i], dzsoil[i], 0.0, dH2O ) ;
            }

        }           /*  end loop on patches  */

    return ;

    }               /*  end sub_vertical()  */


/*--------------------------------------------------------------------------*/

void hydro_routing( struct command_line_object * command_line,
                    double                       extstep,   /*  external time step  */
                    struct basin_object        * basin )
    {
    double      substep ;       /*  subsurface (process-coupling) time step     */
    double      t ;             /*  time-variables (sec) [counts down to 0]     */
    unsigned    i ;
	struct patch_object *   patch ;
    
    if ( num_patches == -9999 )
        {
        init_hydro_routing( command_line, basin ) ;
        }

    /*  copy into working variables  */

#pragma omp parallel for  default( none )                   \
        private( i, patch )                                 \
         shared( num_patches, plist, waterz,                \
                 sfcH2O, sfcNO3, sfcNH4, sfcDOC, sfcDON,    \
                 totH2O, totNO3, totNH4, totDOC, totDON )

    for ( i = 0; i < num_patches; i++ )
        {
        patch     = plist[i] ;
        sfcH2O[i] = patch->detention_store ;    /*  to working vbles for sfc routing  */
        sfcNO3[i] = patch->surface_NO3 ;
        sfcNH4[i] = patch->surface_NH4 ;
        sfcDOC[i] = patch->surface_DOC ;
        sfcDON[i] = patch->surface_DON ;

        waterz[i] = patch->z - ( patch->sat_deficit_z > ZERO ? patch->sat_deficit_z : ZERO ) ;

        totH2O[i] = patch->field_capacity - patch->sat_deficit  ;
        totNO3[i] = patch->soil_ns.nitrate ;
        totNH4[i] = patch->soil_ns.sminn ;
        totDON[i] = patch->soil_ns.DON ;
        totDOC[i] = patch->soil_cs.DOC ;

        }


    /*  main processing loop:  */

    for( t = extstep; t > EPSILON; t-=substep )     /*  counts down to 0, with 10 usec tolerance for roundoff  */
        {
        sub_routing( t, &substep ) ;                /*  sets substep   */

        can_routing( substep ) ;

        sfc_routing( substep ) ;

        stream_routing( substep ) ;

        sub_vertical( substep ) ;
        }


    /*  copy back into model-state  */

#pragma omp parallel for  default( none )                   \
        private( i, patch )                                 \
         shared( num_patches, plist, waterz,                \
                 sfcH2O, sfcNO3, sfcNH4, sfcDOC, sfcDON,    \
                 totH2O, totNO3, totNH4, totDOC, totDON )

    for ( i = 0; i < num_patches; i++ )
        {
        patch                  =  plist[i] ;
        patch->detention_store = sfcH2O[i] ;    /*  from working vbles for sfc routing  */
        patch->surface_NO3     = sfcNO3[i] ;
        patch->surface_NH4     = sfcNH4[i] ;
        patch->surface_DOC     = sfcDOC[i] ;
        patch->surface_DON     = sfcDON[i] ;
        
        patch->sat_deficit_z   = patch->z - waterz[i]  ;
        patch->sat_deficit     = patch->field_capacity - totH2O[i] ;
        patch->soil_ns.nitrate = totNO3[i] ;
        patch->soil_ns.sminn   = totNH4[i] ;
        patch->soil_ns.DON     = totDON[i] ;
        patch->soil_cs.DOC     = totDOC[i] ;
        }

    return ;

    }           /*  end hydro_routing()  */
