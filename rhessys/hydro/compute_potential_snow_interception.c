/*--------------------------------------------------------------*/
/* 								*/
/*		compute_potential_snow_interception		*/
/*								*/
/*								*/
/*	NAME							*/
/*	compute_potential_snow__interception  - computes amount	*/
/*		snow that can be intercepted by the canopy.	*/
/*								*/
/*	SYNOPSIS						*/
/*	compute_potential_interception(   			*/
/*			int	,				*/	
/*			double	,				*/
/*			struct	canopy_strata_object	*);	*/
/*								*/
/*	returns:						*/
/*	potential_interceptione (m) - amount of snow that can be*/
/*		intercepted by the canopy.			*/
/*								*/
/*	OPTIONS							*/
/*	snow (m) - amount of snow on stratum for whole day	*/
/*	canopy_strata_object - state of canopy strata		*/
/*								*/
/*	DESCRIPTION						*/
/*								*/
/*	Computes the amount of snow that is interceptible	*/
/*	given the snow during the day and the current canopy	*/
/*	start of day storage.  Note that this interceptible	*/
/*	snow may be later dripped or evaporated if the code 	*/
/*	choses in compute_snow_stored.				*/
/*								*/
/*	PROGRAMMER NOTES					*/
/*								*/
/* Sept. 14	C. Tague					*/
/*	- include a snow  interception coefficient		*/
/*	(different from rain interception coeff)		*/
/*								*/
/*--------------------------------------------------------------*/
#include <stdio.h>
#include "rhessys.h"
#include "phys_constants.h"

double	compute_potential_snow_interception(
											int	verbose_flag,
											double	snow,
											struct	canopy_strata_object	*stratum)
{
	/*------------------------------------------------------*/
	/*	Local Function Declarations.						*/
	/*------------------------------------------------------*/
	
	/*------------------------------------------------------*/
	/*	Local Variable Definition. 							*/
	/*------------------------------------------------------*/
	double	potential_interception;
	double interception_coef;
	/*--------------------------------------------------------------*/
	/*	Compute amount potentially intercepted.			*/
	/*								*/
	/*	Defined as the minimum of the amount the stratum can cat*/
	/*	and the amount that fell from the stratum (or sky) above*/
	/*--------------------------------------------------------------*/
	
	/*--------------------------------------------------------------*/
	/*	Compute amount potentially intercepted.			*/
	/*	m = m2PlANT / m2ground *  ( (kg  / m2 * day * m2PLANT )	*/
	/*		* ( 1 m3 H20 / 1000 kg H20 )			*/
	/*	limit rain interception by gap_fraction			*/
	/*--------------------------------------------------------------*/
	interception_coef = (1.0 - stratum[0].gap_fraction);
	if (stratum[0].defaults[0][0].epc.veg_type != NON_VEG) 
		potential_interception = MIN(interception_coef * snow,
			stratum[0].epv.all_pai
			* stratum[0].defaults[0][0].specific_snow_capacity
			- stratum[0].snow_stored);
	else
		potential_interception = MIN (snow,
			(stratum[0].defaults[0][0].specific_snow_capacity
			- stratum[0].snow_stored));
		
	potential_interception = MAX(potential_interception, 0.0);
	return( potential_interception );
} /*end compute_potential_snow_interception */
